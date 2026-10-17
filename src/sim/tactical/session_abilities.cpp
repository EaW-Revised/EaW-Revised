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
#include <bit>
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

core::Result<void> TacticalSession::Impl::service_healing(std::vector<LiveUnit>& units,
    const std::map<EntityId, ArrivalState>& arriving, const std::uint64_t tick,
    const PartitionExecutor& executor, std::vector<CombatEvent>& events) {
    if (std::none_of(abilities.profiles.begin(), abilities.profiles.end(), [](const auto& profile) {
        return std::any_of(profile.special.begin(), profile.special.end(), [](const auto& handler) {
            return handler.kind == SpecialAbilityKind::force_healing;
        });
    })) return core::Result<void>::success();
    auto& scratch = healing_scratch;
    scratch.fractions.resize(units.size());
    scratch.errors.assign(units.size(), std::nullopt);
    for (auto& output : scratch.outputs) output.clear();
    scratch.pulses.clear(); scratch.jobs.clear();
    // Immutable health inputs, retained storage; source tasks change only their own deadlines.
    auto phase = executor.execute_phase("healing-inputs", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, units.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& unit = units[i];
            const auto* profile = health_profile(unit);
            scratch.fractions[i] = unit.durability && profile && unit.durability->hull.raw() > 0
                && !arriving.contains(unit.state.entity_id) ? healing_fraction(*profile, *unit.durability)
                : math::Fixed::from_raw(math::Fixed::scale);
        }
    });
    if (!phase) return phase;
    const auto in_range = [&](const LiveUnit& source, const LiveUnit& target, const math::Fixed reach) {
        // WSH-05, ABF-02: inclusive planar centre first, then the rolled model box.
        if (within_range(source.state.position, target.state.position, reach, RangeMetric::planar)) return true;
        const auto* profile = combat.find(target.state.type_id);
        if (profile && profile->collision) {
            const auto transform = unit_transform(target);
            if (!transform) return false;
            detail::CombatUnit geometry;
            geometry.profile = profile;
            geometry.transform = transform.value();
            const auto overlap = detail::collision_overlaps_sphere(geometry, source.state.position, reach);
            return overlap && overlap.value();
        }
        // WSH-05 inferred fallback: absent height bounds conservatively require equal Z.
        const auto* footprint = motion.footprint(target.state.type_id);
        if (!footprint || source.state.position.z != target.state.position.z) return false;
        const auto basis = math::to_matrix(target.state.rotation, {});
        if (!basis) return false;
        const math::Vec2 delta{math::Fixed::from_raw(source.state.position.x.raw() - target.state.position.x.raw()),
            math::Fixed::from_raw(source.state.position.y.raw() - target.state.position.y.raw())};
        math::Fixed x{}, y{};
        const auto& rows = basis.value().rows;
        if (!math::try_dot(delta, {rows[0][0], rows[1][0]}, x)
            || !math::try_dot(delta, {rows[0][1], rows[1][1]}, y)) return false;
        const math::Vec3 closest{math::Fixed::from_raw(std::max<std::int64_t>(std::abs(x.raw()) - footprint->x_extent.raw(), 0)),
            math::Fixed::from_raw(std::max<std::int64_t>(std::abs(y.raw()) - footprint->y_extent.raw(), 0)), {}};
        return within_range({}, closest, reach, RangeMetric::planar);
    };
    phase = executor.execute_phase("healing-select", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, units.size());
        auto& output = scratch.outputs[partition];
        for (auto i = range.begin; i < range.end; ++i) {
            auto& source = units[i];
            if (!source.abilities || !source.durability || source.durability->hull.raw() <= 0
                || arriving.contains(source.state.entity_id) || source.abilities->special.service_cancelled) continue;
            const auto& handlers = abilities.find(source.state.type_id)->special;
            for (std::size_t slot = 0; slot < handlers.size(); ++slot) {
                const auto& handler = handlers[slot];
                auto& state = source.abilities->special.slots[slot];
                if (handler.kind != SpecialAbilityKind::force_healing || handler.style != SpecialActivationStyle::space_automatic
                    || !state.enabled || state.cancelled || tick < state.next_service_frame) continue;
                // WSH-06: a due empty scan advances too; arrivals never restart the interval.
                state.next_service_frame = tick + handler.heal_interval_frames;
                if (handler.heal_amount.raw() <= 0 && handler.heal_percent.raw() <= 0) continue;
                if (handler.heal_range.raw() == 0) {
                    if (scratch.fractions[i].raw() < math::Fixed::scale) output.push_back({i, i, slot});
                    continue;
                }
                std::size_t picked = units.size();
                auto lowest = math::Fixed::from_raw(math::Fixed::scale);
                for (std::size_t target = 0; target < units.size(); ++target) {
                    const auto& candidate = units[target];
                    const auto* fighting = combat.find(candidate.state.type_id);
                    if (scratch.fractions[target].raw() >= math::Fixed::scale || !fighting
                        || !allied(source.state.owner, candidate.state.owner)
                        || !special_type_matches(handler.filter, candidate.state.type_id, fighting->category_bits)
                        || !in_range(source, candidate, handler.heal_range)) continue;
                    if (!handler.single_target_heal) output.push_back({i, target, slot});
                    else if (scratch.fractions[target] < lowest) { lowest = scratch.fractions[target]; picked = target; }
                }
                // WSH-02 query-tie seam: stable entity order, inferred for equal fractions.
                if (picked != units.size()) output.push_back({i, picked, slot});
            }
        }
    });
    if (!phase) return phase;
    for (const auto& output : scratch.outputs) scratch.pulses.insert(scratch.pulses.end(), output.begin(), output.end());
    std::sort(scratch.pulses.begin(), scratch.pulses.end(), [](const auto& a, const auto& b) {
        return std::tie(a.target, a.source, a.slot) < std::tie(b.target, b.source, b.slot);
    });
    for (std::size_t begin = 0; begin < scratch.pulses.size();) {
        std::size_t end = begin + 1;
        while (end < scratch.pulses.size() && scratch.pulses[end].target == scratch.pulses[begin].target) ++end;
        scratch.jobs.push_back({scratch.pulses[begin].target, begin, end}); begin = end;
    }
    phase = executor.execute_phase("healing-health", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, scratch.jobs.size());
        for (auto j = range.begin; j < range.end; ++j) {
            const auto& job = scratch.jobs[j];
            auto& target = units[job.target];
            for (auto p = job.begin; p < job.end; ++p) {
                auto& pulse = scratch.pulses[p];
                const auto& handler = abilities.find(units[pulse.source].state.type_id)->special[pulse.slot];
                const auto result = heal_health(*health_profile(target), *target.durability, handler.heal_percent, handler.heal_amount);
                if (!result) { scratch.errors[job.target] = result.error(); break; }
                pulse.hardpoint = result.value();
            }
        }
    });
    if (!phase) return phase;
    for (const auto& error : scratch.errors) if (error) return core::Result<void>::failure(*error);
    for (const auto& pulse : scratch.pulses) {
        const auto& source = units[pulse.source].state;
        const auto& target = units[pulse.target].state;
        events.push_back({tick, CombatEventKind::heal_pulse, source.entity_id, static_cast<std::uint32_t>(pulse.slot),
            target.entity_id, pulse.hardpoint, source.position, target.position});
    }
    return core::Result<void>::success();
}

bool TacticalSession::Impl::buzz_request_ready(const LiveUnit& source, const math::Vec3 point,
    const std::uint64_t tick) const {
    const auto* profile = abilities.find(source.state.type_id);
    const auto index = profile ? ability_slot(*profile, AbilityKind::buzz_droids) : std::nullopt;
    if (!index || !source.abilities || !profile->abilities[*index].buzz) return false;
    const auto& ability = profile->abilities[*index];
    const auto& buzz = *ability.buzz;
    return ability_ready(ability, source.abilities->slots[*index], ability_gate(source, tick), tick)
        && durability.find(buzz.droid_type) != nullptr
        && within_range(source.state.position, point, buzz.max_range, RangeMetric::planar)
        && (buzz.min_range.raw() <= 0
            || !within_range(source.state.position, point, buzz.min_range, RangeMetric::planar));
}

LiveUnit TacticalSession::Impl::create_buzz_droids(LiveUnit& source, const math::Vec3 point,
    const std::uint64_t tick, EntityId& allocated_next_id) {
    const auto& profile = *abilities.find(source.state.type_id);
    const auto index = *ability_slot(profile, AbilityKind::buzz_droids);
    const auto& ability = profile.abilities[index];
    auto& slot = source.abilities->slots[index];
    // WAB-71/FS-08: creation and full recharge belong to successful application.
    slot.started_tick = tick;
    slot.ready_tick = tick + ability.recharge_frames;
    source.abilities->pending_buzz_position.reset();
    source.state.order = Order{OrderKind::stop, tick};
    const UnitState droid{allocated_next_id, ability.buzz->droid_type, source.state.owner, point, math::identity_quat(), {}};
    allocated_next_id = allocated_next_id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : allocated_next_id + 1;
    adjust_owned(droid, true);
    buzz_droids.push_back({droid.entity_id, profile.type_id, point, tick + ability.buzz->activation_frames});
    return new_unit(droid, tick);
}

const SpecialAbilityProfile* TacticalSession::Impl::beam_profile(const LiveUnit& unit, const AbilityKind kind) const {
    const auto* profile = abilities.find(unit.state.type_id);
    const auto slot = profile ? ability_slot(*profile, kind) : std::nullopt;
    if (!slot) return nullptr;
    const auto expected = kind == AbilityKind::energy_weapon ? SpecialAbilityKind::energy_weapon
        : kind == AbilityKind::leech_shields ? SpecialAbilityKind::leech_shields : SpecialAbilityKind::tractor_beam;
    const auto& name = profile->abilities[*slot].gui_activated_ability_name;
    const auto found = std::find_if(profile->special.begin(), profile->special.end(), [&](const auto& nested) {
        return nested.kind == expected && nested.name == name;
    });
    return found == profile->special.end() ? nullptr : &*found;
}

bool TacticalSession::Impl::beam_target_valid(const LiveUnit& source, const LiveUnit& target,
    const SpecialAbilityProfile& nested, const bool admission) const {
    const auto* profile = combat.find(target.state.type_id);
    return target.durability && target.durability->hull.raw() > 0 && profile != nullptr
        && players_hostile(snapshot_players, source.state.owner, target.state.owner)
        && !(target.nebula && target.nebula->present)
        && special_type_matches(nested.filter, target.state.type_id, profile->category_bits)
        && (nested.kind != SpecialAbilityKind::energy_weapon || profile->living_projectile_collision)
        && (nested.kind != SpecialAbilityKind::leech_shields || !admission || (target.durability->shields.raw() > 0
            && shields_online(*health_profile(target), *target.durability)))
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

bool TacticalSession::Impl::beam_usable(const LiveUnit& source, const AbilityKind kind, const std::uint64_t tick) const {
    const auto* nested = beam_profile(source, kind);
    if (!source.abilities || !nested) return false;
    const auto& profile = *abilities.find(source.state.type_id);
    const auto slot = *ability_slot(profile, kind);
    const auto& special = source.abilities->special.slots[static_cast<std::size_t>(nested - profile.special.data())];
    return !source.abilities->special.service_cancelled && !special.cancelled && nested->initially_enabled != false
        && nested->style == SpecialActivationStyle::user_input
        && ability_ready(profile.abilities[slot], source.abilities->slots[slot], ability_gate(source, tick), tick);
}

AbilitySwitch TacticalSession::Impl::lock_beam(LiveUnit& source, const EntityId target, const AbilityKind kind,
    const std::uint64_t tick) const {
    const auto* nested = beam_profile(source, kind);
    const auto& profile = *abilities.find(source.state.type_id);
    const auto slot = *ability_slot(profile, kind);
    auto& state = source.abilities->slots[slot];
    const auto switched = activate_ability(profile.abilities[slot], state, ability_gate(source, tick), tick);
    source.abilities->replan_due = source.abilities->replan_due || switched.speed;
    state.target = target;
    state.target_hardpoint = no_hardpoint;
    auto& special = source.abilities->special.slots[static_cast<std::size_t>(nested - profile.special.data())];
    special.enabled = true;
    special.targets = {target};
    special.next_service_frame = tick + nested->service_interval;
    source.formation.reset();
    return switched;
}

std::uint32_t TacticalSession::Impl::energy_hardpoint(const LiveUnit& source, const LiveUnit& target) const {
    std::uint32_t chosen = no_hardpoint;
    const auto* profile = combat.find(target.state.type_id);
    const auto transform = unit_transform(target);
    if (!profile || !transform) return chosen;
    std::optional<math::Fixed> nearest;
    for (const auto& hardpoint : profile->hardpoints) {
        if (!hardpoint.targetable || !target_hardpoint_standing(target, hardpoint.hardpoint)) continue;
        const auto position = math::transform_point(transform.value(), hardpoint.position);
        if (!position) continue;
        const auto distance = math::length(math::Vec3{
            math::Fixed::from_raw(position.value().x.raw() - source.state.position.x.raw()),
            math::Fixed::from_raw(position.value().y.raw() - source.state.position.y.raw()),
            math::Fixed::from_raw(position.value().z.raw() - source.state.position.z.raw())});
        if (distance && (!nearest || distance.value() < *nearest)) {
            nearest = distance.value();
            chosen = hardpoint.hardpoint;
        }
    }
    return chosen;
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
        return ability_slot(profile, AbilityKind::energy_weapon) || ability_slot(profile, AbilityKind::tractor_beam)
            || ability_slot(profile, AbilityKind::leech_shields);
    });
    if (!present) return core::Result<void>::success();
    std::array<std::vector<ConcentrateEffect>, tick_partition_count> effects;
    std::array<std::vector<EntityId>, tick_partition_count> replans;
    std::array<std::vector<EntityId>, tick_partition_count> leech_ended;
    const auto serviced = executor.execute_phase("hero-beams", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, units.size());
        for (auto i = range.begin; i < range.end; ++i) {
            auto& source = units[i];
            if (!source.abilities) continue;
            for (const auto kind : {AbilityKind::energy_weapon, AbilityKind::tractor_beam, AbilityKind::leech_shields}) {
                const auto* nested = beam_profile(source, kind);
                if (!nested) continue;
                const auto& profile = *abilities.find(source.state.type_id);
                const auto slot = *ability_slot(profile, kind);
                const auto& state = source.abilities->slots[slot];
                const bool leech_locked = kind == AbilityKind::leech_shields && state.target != invalid_entity_id;
                const auto& special = source.abilities->special.slots[static_cast<std::size_t>(nested - profile.special.data())];
                const auto target = std::lower_bound(units.begin(), units.end(), state.target,
                    [](const auto& unit, const EntityId id) { return unit.state.entity_id < id; });
                if (!state.active || source.abilities->special.service_cancelled || !special.enabled || special.cancelled
                    || !ability_hardpoints_online(profile.abilities[slot], ability_gate(source, tick)) // WHD-02
                    || (kind == AbilityKind::leech_shields
                        && !ability_ready(profile.abilities[slot], state, ability_gate(source, tick), tick)) // WLS-03, WHZ-25
                    || target == units.end() || target->state.entity_id != state.target
                    || !beam_target_valid(source, *target, *nested, false) || !beam_in_range(source, *target, *nested)
                    || (state.target_hardpoint != no_hardpoint && !target_hardpoint_standing(*target, state.target_hardpoint)))
                    release_beam(source, kind, tick);
                // WLS-02: recompute the owner's category-zero bonus after expiry or release.
                if (leech_locked && state.target == invalid_entity_id) leech_ended[partition].push_back(source.state.entity_id);
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
    for (const auto& part : leech_ended) concentrate_changed_targets.insert(part.begin(), part.end());
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

std::vector<const SpecialAbilityProfile*> TacticalSession::Impl::redirect_blaster_handlers(const LiveUnit& defender,
    const LiveUnit& attacker, const std::vector<PlayerEconomy>& staged_ledgers) const {
    std::vector<const SpecialAbilityProfile*> result;
    const auto* profile = abilities.find(defender.state.type_id);
    if (profile == nullptr) return result;
    const auto* attacker_combat = combat.find(attacker.state.type_id);
    const std::uint64_t categories = attacker_combat != nullptr ? attacker_combat->category_bits : 0;
    const auto lower = [](std::string text) {
        for (auto& character : text) {
            if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
        }
        return text;
    };
    for (const auto& special : profile->special) {
        if (special.kind != SpecialAbilityKind::redirect_blaster) continue;
        if (!special_type_matches(special.filter, attacker.state.type_id, categories)) continue;
        bool enabled = special.initially_enabled.value_or(true);
        if (!enabled) {
            const auto name = lower(special.name);
            for (const auto& account : staged_ledgers) {
                const bool own = account.player == defender.state.owner;
                if (!own && !allied(account.player, defender.state.owner)) continue;
                for (const auto& held : account.completed) {
                    const auto* upgrade = economy.upgrade(held.type);
                    if (upgrade == nullptr) continue;
                    for (const auto& rule : upgrade->ability_enables) {
                        if ((!own && !rule.all_allies) || rule.ability_name != name) continue;
                        if (std::binary_search(rule.applicable.begin(), rule.applicable.end(), defender.state.type_id)) enabled = true;
                    }
                }
            }
        }
        if (enabled) result.push_back(&special);
    }
    return result;
}

core::Result<void> TacticalSession::Impl::service_laser_defence(const std::vector<LiveUnit>& units,
    const std::uint64_t tick, const PartitionExecutor& executor, std::vector<CombatEvent>& zaps) {
    if (projectiles.empty() || std::none_of(abilities.profiles.begin(), abilities.profiles.end(), [](const auto& profile) {
        return ability_slot(profile, AbilityKind::laser_defense).has_value();
    })) return core::Result<void>::success();
    // Immutable inputs, one slot per unit: the handler of a unit whose look is due this frame.
    std::vector<const SpecialAbilityProfile*> due(units.size());
    const auto prepared = executor.execute_phase("laser-defence-due", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, units.size());
        for (auto index = range.begin; index < range.end; ++index) {
            const auto& unit = units[index];
            if (!unit.abilities) continue;
            const auto& profile = *abilities.find(unit.state.type_id);
            const auto slot = ability_slot(profile, AbilityKind::laser_defense);
            if (!slot) continue;
            const auto& state = unit.abilities->slots[*slot];
            // WLD-03: serviced on every frame after the switch-on; the first service looks at once and the
            // counter then runs a recharge-time of frames, so a look comes every look_frames + 1 services.
            if (!state.active || tick <= state.started_tick) continue;
            const auto& name = profile.abilities[*slot].gui_activated_ability_name;
            const auto handler = std::find_if(profile.special.begin(), profile.special.end(), [&](const auto& nested) {
                return nested.kind == SpecialAbilityKind::laser_defense && nested.name == name;
            });
            if (handler == profile.special.end()) continue;
            if ((tick - state.started_tick - 1) % (static_cast<std::uint64_t>(handler->defence_look_frames) + 1U) != 0) continue;
            due[index] = &*handler;
        }
    });
    if (!prepared) return prepared;
    const auto matches = [&](const LiveUnit& unit, const SpecialAbilityProfile& handler, const Projectile& shot) {
        // WLD-04: another owner that is not an ally, a targeted category, flown farther than the minimum
        // and inside the protection radius around the unit.
        return !shot.explosion_requested && shot.owner != unit.state.owner && !allied(unit.state.owner, shot.owner)
            && (projectile_defence_category(shot) & handler.defence_categories) != 0
            && shot.travelled.raw() > handler.defence_min_travel.raw()
            && within_range(unit.state.position, shot.position, handler.defence_radius, RangeMetric::spatial);
    };
    // Each unit reads the projectile list and writes only its own slot.
    std::vector<std::size_t> picked(units.size(), projectiles.size());
    const auto searched = executor.execute_phase("laser-defence-look", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, units.size());
        for (auto index = range.begin; index < range.end; ++index) {
            if (due[index] == nullptr) continue;
            for (std::size_t shot = 0; shot < projectiles.size(); ++shot) {
                if (!matches(units[index], *due[index], projectiles[shot])) continue;
                picked[index] = shot;
                break;
            }
        }
    });
    if (!searched) return searched;
    // WLD-05: one shot per look, the first in projectile order. An earlier unit's zap this frame removes its
    // shot from the later units' search, so a conflict re-reads the list past the shot just taken.
    for (std::size_t index = 0; index < units.size(); ++index) {
        if (due[index] == nullptr || picked[index] >= projectiles.size()) continue;
        std::size_t shot = picked[index];
        while (shot < projectiles.size() && !matches(units[index], *due[index], projectiles[shot])) ++shot;
        if (shot >= projectiles.size()) continue;
        auto& target = projectiles[shot];
        target.explosion_requested = true; // WLD-05: it detonates where it is, then is gone (WAD-05)
        zaps.push_back(CombatEvent{tick, CombatEventKind::point_defence_zap, units[index].state.entity_id, 0, target.id,
            no_hardpoint, units[index].state.position, target.position, 0, target.target});
    }
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
            math::Fixed::from_raw(-nested->target_damage_increase.raw()),
            math::Fixed::from_raw(-nested->target_speed_decrease.raw())});
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
                math::Fixed::from_raw(-nested->target_damage_increase.raw()),
                math::Fixed::from_raw(-nested->target_speed_decrease.raw())});
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

const BuzzDroidsProfile* TacticalSession::Impl::buzz_profile(const TypeId source) const {
    const auto* profile = abilities.find(source);
    const auto slot = profile ? ability_slot(*profile, AbilityKind::buzz_droids) : std::nullopt;
    return slot && profile->abilities[*slot].buzz ? &*profile->abilities[*slot].buzz : nullptr;
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
    return sum_combat_bonus_categories(categories, targeted || command_ledger.recipients.contains(target.state.entity_id) || command_ledger.battlefield_defence.contains(target.state.owner))[4];
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
        gate.corrupted = unit.corrupt.has_value();
        gate.in_nebula = unit.nebula && unit.nebula->frame && unit.nebula->present;
        if (!unit.durability) return gate;
        const auto& profile = *health_profile(unit);
        gate.hardpoint_profile = &profile;
        gate.hardpoint_state = &*unit.durability;
        if (const auto* modes = abilities.find(unit.state.type_id)) {
            for (const auto index : modes->gravity_well_hardpoints) {
                if (hardpoint_destroyed(profile, *unit.durability, index)) gate.gravity_well_online = false;
            }
        }
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
    unit.take_damage_mode = retreat_escape(unit.state.entity_id) ? math::Fixed{} : ability_factor(unit, AbilityModifier::take_damage);
}

void TacticalSession::Impl::defend_switched_on(LiveUnit& unit, const std::uint64_t tick) const {
        if (!unit.durability || !durability.damage) return;
        unit.durability->next_shield_frame = tick + 1;
        if (has_energy_pool(*health_profile(unit), *durability.damage)) {
            unit.durability->next_energy_frame = tick + 1;
        }
    }

void TacticalSession::Impl::track_damage(LiveUnit& unit, const math::Fixed hull_before, const math::Fixed shields_before) const {
        // WCC-54, WCC-82: every DAMAGE_TRACKING unit sums the hull and shield each hit took.
        if (!unit.durability) return;
        const auto* profile = health_profile(unit);
        if (profile == nullptr || !profile->damage_tracking) return;
        const auto before = hull_before.raw() + shields_before.raw();
        const auto after = unit.durability->hull.raw() + unit.durability->shields.raw();
        if (before > after) {
            unit.durability->window_damage = math::Fixed::from_raw(unit.durability->window_damage.raw() + (before - after));
        }
    }

void TacticalSession::Impl::ion_stun_unit(LiveUnit& unit, const IonStunShot& shot, const std::uint64_t tick) const {
        unit.ion_stun = ion_stun(unit.ion_stun, shot, tick);
        if (!unit.abilities) return;
        clear_stealth(unit); // WST-06
        const auto& profile = *abilities.find(unit.state.type_id);
        const auto slot = ability_slot(profile, AbilityKind::defend);
        if (!slot) return;
        const auto switched = deactivate_ability(profile.abilities[*slot], unit.abilities->slots[*slot], tick);
        unit.abilities->replan_due = unit.abilities->replan_due || switched.speed;
        if (switched.changed) refresh_damage_modes(unit);
    }

void TacticalSession::Impl::stealth_switched_on(LiveUnit& unit, const std::uint64_t tick, const math::Fixed factor) const {
    // WST-03: the cloak goes on undetected; the unit's attack target and its hardpoints' targets
    // are dropped and an order on a unit ends, keeping the move under way (as HUNT, WAB-04).
    unit.abilities->stealth_on = true;
    unit.abilities->stealth_detected = 0;
    // WST-03/04: its own count is trunc(Expiration_Seconds x factor x 30) frames; only where that
    // differs from the slot's countdown does the cloak keep its own end.
    unit.abilities->stealth_end = 0;
    const auto& profile = *abilities.find(unit.state.type_id);
    if (const auto slot = ability_slot(profile, AbilityKind::stealth)) {
        const auto& ability = profile.abilities[*slot];
        // A profile without authored seconds (built from frames alone) scales its frame count.
        const auto frames = ability.expiration_seconds.raw() > 0 ? stealth_frames(ability.expiration_seconds, factor)
            : (factor.raw() > 0 ? (std::uint64_t{ability.expiration_frames} * static_cast<std::uint64_t>(factor.raw()))
                    >> math::Fixed::fractional_bits : std::uint64_t{ability.expiration_frames});
        if (frames != ability.expiration_frames) unit.abilities->stealth_end = tick + frames;
    }
    if (unit.combat) {
        unit.combat->attack_target = invalid_entity_id;
        unit.combat->attack_hardpoint = no_hardpoint;
        unit.combat->direct = false;
        for (auto& weapon : unit.combat->weapons) weapon.opportunity.target = invalid_entity_id;
    }
    if (unit.state.order.target != invalid_entity_id) unit.state.order = {};
}

void TacticalSession::Impl::clear_stealth(LiveUnit& unit) noexcept {
    if (!unit.abilities) return;
    unit.abilities->stealth_on = false;
    unit.abilities->stealth_detected = 0;
    unit.abilities->stealth_end = 0;
}

math::Fixed TacticalSession::Impl::stealth_duration_factor(const PlayerId owner, const std::vector<PlayerEconomy>& accounts) const {
    // WST-03: an upgrade's modifier reaches its buyer, and the buyer's allies with Apply_To_All_Allies.
    std::vector<std::pair<std::uint32_t, math::Fixed>> largest; // stacking category, its largest contribution
    for (const auto& account : accounts) {
        const bool own = account.player == owner;
        if (!own && !allied(account.player, owner)) continue;
        for (const auto& held : account.completed) {
            const auto* upgrade = economy.upgrade(held.type);
            if (upgrade == nullptr) continue;
            for (const auto& modifier : upgrade->stealth_duration) {
                if (!own && !modifier.all_allies) continue;
                const auto found = std::find_if(largest.begin(), largest.end(),
                    [&](const auto& entry) { return entry.first == modifier.stacking_category; });
                if (found == largest.end()) largest.emplace_back(modifier.stacking_category, modifier.percentage);
                else found->second = std::max(found->second, modifier.percentage);
            }
        }
    }
    auto total = math::Fixed::scale; // 1 plus the category winners
    for (const auto& entry : largest) total += entry.second.raw();
    return math::Fixed::from_raw(total);
}

void TacticalSession::Impl::detect_stealth(LiveUnit& unit, const std::span<const LiveUnit> units, const std::uint64_t tick) const {
    // WST-08: every frame while on, the first enemy whose type targets stealth units and whose own
    // reveal range strictly contains the cloaked unit (3D) refreshes the detection frame. The other
    // units' positions, owners and types are not written in this phase.
    if (!unit.abilities || !unit.abilities->stealth_on) return;
    for (const auto& other : units) {
        if (other.state.entity_id == unit.state.entity_id
            || !players_hostile(snapshot_players, unit.state.owner, other.state.owner)) continue;
        const auto* profile = combat.find(other.state.type_id);
        if (profile == nullptr || profile->stealth_detection_range.raw() <= 0) continue;
        if (strictly_within_range(other.state.position, unit.state.position, profile->stealth_detection_range, RangeMetric::spatial)) {
            unit.abilities->stealth_detected = tick;
            return;
        }
    }
}

bool TacticalSession::Impl::hidden_from(const UnitStage& staged, const EntityId target, const LiveUnit& attacker,
    const std::uint64_t tick) const {
    const auto members_of = [&](const EntityId id) -> const std::vector<EntityId>* {
        const auto found = std::lower_bound(squadrons.begin(), squadrons.end(), id,
            [](const Squadron& squadron, const EntityId value) { return squadron.container < value; });
        return found != squadrons.end() && found->container == id ? &found->members : nullptr;
    };
    const auto* shooter = combat.find(attacker.state.type_id);
    if (shooter == nullptr) {
        if (const auto* members = members_of(attacker.state.entity_id); members != nullptr) {
            for (const auto member : *members) {
                if (const auto craft = staged.find(member); craft != staged.end()) {
                    shooter = combat.find(craft->second.state.type_id);
                    break;
                }
            }
        }
    }
    if (shooter != nullptr && shooter->target_stealth) return false;
    const auto invisible = [&](const EntityId id) {
        const auto found = staged.find(id);
        return found != staged.end() && found->second.state.owner != attacker.state.owner
            && found->second.abilities && stealth_invisible(*found->second.abilities, tick,
                abilities.find(found->second.state.type_id));
    };
    if (const auto* members = members_of(target); members != nullptr) {
        bool any = false;
        for (const auto member : *members) {
            if (staged.find(member) == staged.end()) continue;
            if (!invisible(member)) return false;
            any = true;
        }
        return any;
    }
    return invisible(target);
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

namespace {

// WSD-09: the Krayt object script's constants (its data file): the proximity range, the AI's threat
// trigger and the human autofire's hull and neighbour thresholds. Its ServiceRate is 1 s.
constexpr std::int64_t krayt_range = 400;
constexpr std::int64_t krayt_threat_trigger = 1500;
constexpr std::uint32_t krayt_human_neighbours = 2;
constexpr std::int64_t krayt_service_rate = std::int64_t{1} << 28; // 1 s on the script clock

// WFO-22: the clock an object script's service compares, the frame count times the single-precision
// 1/30 (0x888889 x 2^-28), rounded to single precision, in units of 2^-28 s. The integer rounding is
// the float's (ADR-010), so successive services fall 31 frames apart, now and then 30 (FH-11).
[[nodiscard]] std::int64_t script_clock(const std::int64_t frame) noexcept {
    const auto single = [](std::uint64_t value) {
        const auto width = std::bit_width(value);
        if (width <= 24) return value;
        const auto shift = static_cast<unsigned>(width - 24);
        auto kept = value >> shift;
        const auto rest = value & ((std::uint64_t{1} << shift) - 1);
        const auto half = std::uint64_t{1} << (shift - 1);
        if (rest > half || (rest == half && (kept & 1U) != 0)) ++kept;
        return kept << shift;
    };
    const auto magnitude = static_cast<std::uint64_t>(frame < 0 ? -frame : frame);
    const auto seconds = static_cast<std::int64_t>(single(single(magnitude) * 0x888889U));
    return frame < 0 ? -seconds : seconds;
}

} // namespace

void TacticalSession::Impl::self_destruct_switched(LiveUnit& unit) {
    if (unit.combat) {
        unit.combat->attack_target = invalid_entity_id;
        unit.combat->attack_hardpoint = no_hardpoint;
        unit.combat->direct = false;
    }
    if (unit.state.order.target != invalid_entity_id) unit.state.order = {};
}

bool TacticalSession::Impl::service_self_destruct(std::vector<LiveUnit>& units, const std::size_t index,
    const std::map<EntityId, EntityId>& craft_squadron, const detail::CollectionTrees& boxes,
    const std::uint64_t tick) const {
    auto& unit = units[index];
    if (!unit.abilities || !unit.durability) return false;
    const auto& profile = *abilities.find(unit.state.type_id);
    const auto kind_slot = ability_slot(profile, AbilityKind::self_destruct);
    if (!kind_slot) return false;
    auto& slot = unit.abilities->slots[*kind_slot];
    // WSD-04: the countdown has run out; the detonation service ends the ability with the unit (before
    // the object's Lua, which a deletion-pending object no longer runs, WFO-22).
    if (slot.active && slot.expires_tick != 0 && tick >= slot.expires_tick) return true;
    if (!profile.self_destruct_script) return false;
    // WFO-22: the script runs in the object's first service, then whenever the clock is more than its
    // ServiceRate past LastService. The first service sets LastService to minus a sync-random share of
    // the rate; the remake spreads that phase by entity ID (unverified draw, as AI-32).
    const auto now = script_clock(static_cast<std::int64_t>(tick));
    auto& script = unit.abilities->self_destruct_script;
    const bool first = !script.has_value();
    if (!first && now - script->last_service <= krayt_service_rate) return false;
    if (first) script.emplace();
    auto& state = *script;
    state.last_service = first ? -script_clock(static_cast<std::int64_t>(unit.state.entity_id % 30)) : now;
    // WSD-09: the state's OnUpdate reads what the proximity callbacks counted at the end of the previous
    // service. The human script's autofire state changes take a service: the state left resets the counts
    // before the state entered runs, in the same service.
    const auto nearby = state.nearby;
    const auto threat = state.threat;
    bool trigger = false;
    // Get_Hull: the display health, the hull share for a type without destroyable hardpoints (FH-23).
    std::optional<math::Fixed> fraction;
    if (const auto* health = health_profile(unit); health != nullptr && health->max_hull.raw() > 0) {
        if (auto share = math::divide(unit.durability->hull, health->max_hull)) fraction = share.value();
    }
    if (abilities.human(unit.state.owner)) {
        const bool counted = state.armed;
        state.armed = slot.autofire;
        // WSD-09: hull at most 20 % and more than two neighbouring objects.
        trigger = counted && slot.autofire && fraction && fraction->raw() <= math::Fixed::scale / 5
            && nearby > krayt_human_neighbours;
    } else if (fraction && fraction->raw() < math::Fixed::scale / 2) {
        // WSD-09: the AI below half hull when the threat nearby reaches 1500 scaled by the hull's share of half.
        const auto share = math::multiply(math::Fixed::from_raw(fraction->raw() * 2),
            math::Fixed::from_raw(krayt_threat_trigger * math::Fixed::scale));
        trigger = share && threat >= share.value();
    }
    if (trigger && !slot.active) {
        const auto switched = activate_ability(profile.abilities[*kind_slot], slot, ability_gate(unit, tick), tick);
        if (switched.changed) self_destruct_switched(unit);
    }
    // WSD-10: the proximity callbacks at the end of the service. The engine collects every collidable
    // whose box touches the cube of 400 around the unit (no fog test); the script keeps the enemies,
    // promotes a craft to its squadron and counts each object once, adding its type's combat rating.
    state.nearby = 0;
    state.threat = {};
    const auto reach = math::Fixed::from_raw(krayt_range * math::Fixed::scale);
    const auto& at = unit.state.position;
    const auto low = [&](const math::Fixed value) { return math::Fixed::from_raw(value.raw() - reach.raw()); };
    const auto high = [&](const math::Fixed value) { return math::Fixed::from_raw(value.raw() + reach.raw()); };
    const detail::CullBox query{{low(at.x), low(at.y), low(at.z)}, {high(at.x), high(at.y), high(at.z)}};
    std::set<EntityId> seen;
    for (const auto& player : snapshot_players) {
        if (!players_hostile(snapshot_players, unit.state.owner, player.player_id)) continue;
        for (const auto id : boxes.collect(player.player_id, query)) {
            const auto parent = craft_squadron.find(id);
            const auto key = parent != craft_squadron.end() ? parent->second : id;
            if (!seen.insert(key).second) continue;
            const auto live = [&](const EntityId wanted) -> const LiveUnit* {
                const auto found = std::lower_bound(units.begin(), units.end(), wanted,
                    [](const LiveUnit& value, const EntityId entity) { return value.state.entity_id < entity; });
                return found != units.end() && found->state.entity_id == wanted ? &*found : nullptr;
            };
            const auto* object = live(key);
            if (object == nullptr) object = live(id);
            if (object == nullptr) continue;
            const auto sum = math::add(state.threat, abilities.combat_rating(object->state.type_id));
            if (!sum) continue;
            ++state.nearby;
            state.threat = sum.value();
        }
    }
    return false;
}

void TacticalSession::Impl::service_blast(LiveUnit& unit, const std::uint64_t tick) const {
    if (!unit.abilities || !unit.abilities->charged_blast || unit.abilities->special.service_cancelled) return;
    const auto& profile = *abilities.find(unit.state.type_id);
    const auto slot = ability_slot(profile, AbilityKind::blast);
    if (!slot) return;
    const auto nested = std::find_if(profile.special.begin(), profile.special.end(), [&](const auto& entry) {
        return entry.kind == SpecialAbilityKind::blast && entry.name == profile.abilities[*slot].gui_activated_ability_name;
    });
    if (nested == profile.special.end()) return;
    auto& handler = unit.abilities->special.slots[static_cast<std::size_t>(nested - profile.special.begin())];
    if (!handler.enabled || handler.cancelled || tick < handler.next_service_frame) return;
    handler.next_service_frame = tick + nested->service_interval;
    std::uint32_t eligible{};
    // FC-62/FD-56: completion reads current gun health/disable only, never target or cooldown.
    if (!unit.abilities->charged_blast->charged_presentation && unit.abilities->charged_blast->charge_remaining <= 1) {
        const auto* guns = combat.find(unit.state.type_id);
        const auto* health = health_profile(unit);
        if (guns) eligible = detail::eligible_blast_specials(*guns, health,
            unit.durability ? &*unit.durability : nullptr);
    }
    service_charged_blast(unit.abilities->charged_blast, eligible);
}

bool TacticalSession::Impl::service_mc30(LiveUnit& unit, const std::span<const UnitState> inputs,
    const std::map<EntityId, EntityId>& craft_squadron, const detail::CollectionTrees& boxes,
    const std::uint64_t tick) const {
    const auto* profile = abilities.find(unit.state.type_id);
    if (!unit.abilities || !profile || !profile->mc30_script) return false;
    const auto kind_slot = ability_slot(*profile, AbilityKind::cluster_bomb);
    if (!kind_slot) return false;
    auto& slot = unit.abilities->slots[*kind_slot];
    auto& script = unit.abilities->mc30_script;
    const auto now = script_clock(static_cast<std::int64_t>(tick));
    const bool first = !script;
    // FC-57: strict > 1 s on the single-precision mode clock, including equality skips.
    if (!first && now - script->last_service <= (std::int64_t{1} << 28)) return false;
    if (first) {
        script.emplace();
        script->human = abilities.human(unit.state.owner);
    }
    auto& state = *script;
    // FC-57/UFS-27: retain the existing deterministic entity phase stand-in for the
    // unverified synchronized random draw; no general lifecycle/reset producer is added.
    state.last_service = first ? -script_clock(static_cast<std::int64_t>(unit.state.entity_id % 30)) : now;
    // FS-29/FSC-46: arming leaves the unarmed state, clearing its old sample first.
    if (state.human && !state.armed && slot.autofire) {
        state.nearby = 0;
        state.threat = {};
    }
    state.armed = state.human && slot.autofire;
    // FS-29: armed human count > 2; AI combat-rating threat >= 1000.
    const bool trigger = state.human ? state.armed && state.nearby > 2
        : state.threat >= math::Fixed::from_raw(1000 * math::Fixed::scale);
    const bool fired = trigger && !slot.active && profile->abilities[*kind_slot].spawned
        && durability.damage && ability_ready(profile->abilities[*kind_slot], slot, ability_gate(unit, tick), tick);
    if (fired) {
        // FD-47: direct instant use in this turn, before the nested proximity sample.
        slot.started_tick = tick;
        slot.ready_tick = tick + profile->abilities[*kind_slot].recharge_frames;
    }
    state.nearby = 0;
    state.threat = {};
    const auto reach = math::Fixed::from_raw(500 * math::Fixed::scale); // FC-58 authored query extent
    const auto low = [&](const math::Fixed value) { return math::Fixed::from_raw(value.raw() - reach.raw()); };
    const auto high = [&](const math::Fixed value) { return math::Fixed::from_raw(value.raw() + reach.raw()); };
    const auto& at = unit.state.position;
    const detail::CullBox query{{low(at.x), low(at.y), low(at.z)}, {high(at.x), high(at.y), high(at.z)}};
    const auto live = [&](const EntityId id) -> const UnitState* {
        const auto found = std::lower_bound(inputs.begin(), inputs.end(), id,
            [](const UnitState& value, const EntityId wanted) { return value.entity_id < wanted; });
        return found != inputs.end() && found->entity_id == id ? &*found : nullptr;
    };
    std::set<EntityId> seen;
    for (const auto& player : snapshot_players) {
        if (!players_hostile(snapshot_players, unit.state.owner, player.player_id)) continue;
        for (const auto id : boxes.collect(player.player_id, query)) {
            const auto parent = craft_squadron.find(id);
            const auto key = parent == craft_squadron.end() ? id : parent->second;
            const auto* object = live(key);
            // FC-58: exclude by promoted position, then deduplicate by promoted identity.
            // There is no Euclidean upper 500-distance test on this box query.
            if (!object || strictly_within_range(at, object->position,
                    math::Fixed::from_raw(150 * math::Fixed::scale), RangeMetric::spatial)
                || !seen.insert(key).second) continue;
            const auto sum = math::add(state.threat, abilities.combat_rating(object->type_id));
            if (!sum) continue;
            ++state.nearby;
            state.threat = sum.value();
        }
    }
    return fired;
}

bool TacticalSession::Impl::service_abilities(LiveUnit& unit, const std::uint64_t tick) const {
        auto& state = *unit.abilities;
        const auto& profile = *abilities.find(unit.state.type_id);
        bool speed = state.replan_due;
        state.replan_due = false;
        speed = expire_abilities(profile, state, tick).speed || speed;
        // WHD-04: generator loss explicitly switches INTERDICT off.
        if (const auto index = ability_slot(profile, AbilityKind::interdict);
            index && !ability_hardpoints_online(profile.abilities[*index], ability_gate(unit, tick))) {
            static_cast<void>(deactivate_ability(profile.abilities[*index], state.slots[*index], tick));
        }
        // WST-04/05: the cloak ends with its slot's time limit (the handler counts the same frames),
        // or at its own end when a duration modifier lengthened it: the slot's expiry leaves it on.
        if (const auto cloak = ability_slot(profile, AbilityKind::stealth); cloak && state.stealth_on
            && (state.stealth_end != 0 ? tick >= state.stealth_end : !state.slots[*cloak].active)) clear_stealth(unit);
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
        // AB-41, AB-42: the object script's check runs when the damage-rate window closes, which the
        // ability phase did for every tracked unit just before (WCC-82).
        if (profile.defend_script && rate_window_closes(tick)) {
            const auto slot = ability_slot(profile, AbilityKind::defend);
            const bool armed = slot && (!abilities.human(unit.state.owner) || state.slots[*slot].autofire);
            const auto rate = unit.durability ? unit.durability->damage_rate : math::Fixed{};
            if (armed && rate.raw() > defend_rate_threshold * math::Fixed::scale) {
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
            status.position = slot.position;
            status.active = slot.active;
            status.ready = ability_ready(ability, slot, gate, tick);
            if (ability.kind == AbilityKind::blast) {
                const auto nested = std::find_if(profile.special.begin(), profile.special.end(), [&](const auto& entry) {
                    return entry.kind == SpecialAbilityKind::blast && entry.name == ability.gui_activated_ability_name;
                });
                if (nested == profile.special.end()) status.ready = false;
                else {
                    const auto& handler = unit.abilities->special.slots[static_cast<std::size_t>(nested - profile.special.begin())];
                    status.ready = status.ready && nested->initially_enabled != false
                        && !unit.abilities->special.service_cancelled && !handler.cancelled;
                }
            }
            status.autofire = slot.autofire;
            status.supports_autofire = ability.supports_autofire;
            if (ability.kind == AbilityKind::blast && unit.abilities->charged_blast) {
                status.blast_charge_remaining = unit.abilities->charged_blast->charge_remaining;
                status.blast_charged_presentation = unit.abilities->charged_blast->charged_presentation;
            }
            if (slot.active && (ability.kind == AbilityKind::energy_weapon || ability.kind == AbilityKind::tractor_beam
                || ability.kind == AbilityKind::leech_shields)) {
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

std::vector<std::size_t> TacticalSession::Impl::radar_slots(const TypeId type) const {
    std::vector<std::size_t> slots;
    if (const auto* profile = abilities.find(type))
        for (std::size_t slot = 0; slot < profile->special.size(); ++slot)
            if (profile->special[slot].kind == SpecialAbilityKind::enable_radar) slots.push_back(slot);
    return slots;
}

core::Result<std::vector<SnapshotPlayer>> TacticalSession::Impl::publish_named_abilities(
    std::vector<LiveUnit>& inputs, const NamedAbilityLocks& locks, const RadarRoster& roster,
    const PartitionExecutor& executor) const {
    auto players = snapshot_players;
    for (auto& player : players) player.radar_enabled = false;
    if (roster.empty()) return core::Result<std::vector<SnapshotPlayer>>::success(std::move(players));
    // One source per unit keeps its slot writes disjoint even with multiple radar handlers.
    std::vector<const RadarRoster::value_type*> sources;
    sources.reserve(roster.size());
    for (const auto& source : roster) sources.push_back(&source);
    std::array<std::vector<std::uint8_t>, tick_partition_count> contributions;
    for (auto& partition : contributions) partition.resize(players.size());
    const auto prepared = executor.execute_phase("named-ability-radar", tick_partition_count,
        [&](const std::size_t partition) {
        const auto range = partition_range(partition, sources.size());
        for (auto index = range.begin; index < range.end; ++index) {
            const auto& [id, slots] = *sources[index];
            const auto found = std::lower_bound(inputs.begin(), inputs.end(), id,
                [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
            if (found == inputs.end() || found->state.entity_id != id) continue;
            auto& unit = *found;
            const auto* profile = abilities.find(unit.state.type_id);
            if (!profile || !unit.abilities || (unit.durability && unit.durability->hull.raw() <= 0)) continue;
            for (const auto slot : slots) {
                const auto& handler = profile->special[slot];
                auto& state = unit.abilities->special.slots[slot];
                // WRAD-04/FC-75: zero-interval automatic effect, driven by lifecycle locks.
                state.enabled = named_ability_enabled(locks, unit.state.owner, unit.state.type_id, handler);
                if (!state.enabled || state.cancelled || unit.abilities->special.service_cancelled
                    || handler.style != SpecialActivationStyle::skirmish_automatic) continue;
                for (std::size_t player = 0; player < players.size(); ++player)
                    if (players[player].player_id == unit.state.owner
                        || (handler.affects_all_allies && allied(unit.state.owner, players[player].player_id)))
                        contributions[partition][player] = 1;
            }
        }
    });
    if (!prepared) return core::Result<std::vector<SnapshotPlayer>>::failure(prepared.error());
    // Ordered reduction of bounded player outputs; no serial per-object work.
    for (const auto& partition : contributions) for (std::size_t player = 0; player < players.size(); ++player)
        players[player].radar_enabled |= partition[player] != 0;
    return core::Result<std::vector<SnapshotPlayer>>::success(std::move(players));
}

} // namespace eawr::sim::tactical
