#include "eawr/sim/tactical/session.hpp"

#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "fighters_internal.hpp"
#include "orders_internal.hpp"
#include "eawr/sim/tactical/formation.hpp"
#include "motion_internal.hpp"
#include "session_tick.hpp"
#include "session_services.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// #3644 A1: the object-turn seam. docs/simulation.md "Phase map" lists the `object-turn-rows` phase.
namespace eawr::sim::tactical {

namespace {

// Held while a provider call runs: the session refuses its public mutators meanwhile.
struct RunningGuard final {
    bool& flag;
    explicit RunningGuard(bool& value) noexcept : flag(value) { flag = true; }
    ~RunningGuard() { flag = false; }
    RunningGuard(const RunningGuard&) = delete;
    RunningGuard& operator=(const RunningGuard&) = delete;
};

} // namespace

// Engine ability kinds whose complete activation path is admitted at the turn seam.
bool object_turn_supports(const AbilityKind kind) noexcept {
    switch (kind) {
    case AbilityKind::defend:
    case AbilityKind::turbo:
    case AbilityKind::power_to_weapons:
    case AbilityKind::spoiler_lock:
    case AbilityKind::invulnerability:
    case AbilityKind::sensor_jamming:
    case AbilityKind::laser_defense:
    case AbilityKind::full_salvo:
    case AbilityKind::self_destruct:
    case AbilityKind::cluster_bomb:
    case AbilityKind::harmonic_bomb:
    case AbilityKind::weaken_enemy:
    case AbilityKind::replenish_wingmen:
    case AbilityKind::hunt:
    case AbilityKind::lure:
    case AbilityKind::stealth:
    case AbilityKind::interdict:
    case AbilityKind::missile_shield:
    case AbilityKind::energy_weapon:
    case AbilityKind::tractor_beam:
    case AbilityKind::leech_shields:
        return true;
    default:
        return false;
    }
}

core::Result<void> TacticalSession::set_object_turn_service(std::shared_ptr<ObjectTurnService> service) {
    if (impl_->object_turn_running) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
            "set_object_turn_service during an object-turn provider call"));
    }
    impl_->object_turn = std::move(service);
    return core::Result<void>::success();
}

core::Result<void> TacticalSession::submit_object_turn(const ObjectTurnRecord& record) {
    const auto context = "object-turn use (tick " + std::to_string(record.tick) + ", unit "
        + std::to_string(record.use.unit) + ", sequence " + std::to_string(record.use.sequence) + ")";
    if (impl_->object_turn_running) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
            context + ": submitted during an object-turn provider call"));
    }
    if (record.use.unit == invalid_entity_id || record.use.ability == AbilityKind::none
        || static_cast<std::uint8_t>(record.use.ability) > static_cast<std::uint8_t>(AbilityKind::blast)
        || static_cast<std::uint8_t>(record.use.action) < static_cast<std::uint8_t>(AbilityAction::activate)
        || static_cast<std::uint8_t>(record.use.action) > static_cast<std::uint8_t>(AbilityAction::autofire_off)) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
            context + ": names no unit, ability or action"));
    }
    if (record.tick >= max_ticks) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            context + ": tick exceeds the tactical tick limit"));
    }
    if (record.tick < impl_->completed_tick) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::late_command,
            context + ": tick has already executed; the next tick to execute is "
                + std::to_string(impl_->completed_tick)));
    }
    if (impl_->executed.size() + impl_->pending.size() + impl_->object_turns_executed.size()
            + impl_->object_turn_pending.size() >= max_commands) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            context + ": session command count limit reached"));
    }
    const auto key = std::tuple{record.tick, record.use.unit, record.use.sequence};
    if (!impl_->object_turn_pending.empty() && !(std::prev(impl_->object_turn_pending.end())->first < key)) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::order,
            context + ": does not follow the previous object-turn use"));
    }
    impl_->object_turn_pending.emplace(key, record);
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::object_turn_seam() {
    const auto first = impl_->object_turn_pending.lower_bound(std::tuple{tick, EntityId{}, std::uint64_t{}});
    const auto last = impl_->object_turn_pending.lower_bound(std::tuple{tick + 1, EntityId{}, std::uint64_t{}});
    const bool recorded = first != last;
    if (!impl_->object_turn && !recorded) return core::Result<void>::success();

    auto& moving = gather_.value().moving.value();
    auto& replans = tracked_.value().replans.value();
    std::vector<ObjectTurnRecord> turn;
    // The provider's turn, only when it reports one due; the rows exist only then.
    if (impl_->object_turn) {
        const RunningGuard guard(impl_->object_turn_running);
        if (impl_->object_turn->due(tick)) {
            if (recorded) {
                return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
                    "tick " + std::to_string(tick) + " has recorded object-turn uses and a due provider"));
            }
            const auto& craft_squadron = craft_prep_.value().craft_squadron.value();
            std::vector<ObjectTurnRow> rows(moving.size());
            const auto staged_rows = executor.execute_phase("object-turn-rows", tick_partition_count, [&](const std::size_t partition) {
                const auto range = partition_range(partition, moving.size());
                for (auto index = range.begin; index < range.end; ++index) {
                    auto& unit = moving[index];
                    auto& row = rows[index];
                    row.state = unit.state;
                    row.published = impl_->instance_for(unit, unit_transform(unit).value(), tick, &special_weapons_);
                    row.attack_target = unit.combat ? unit.combat->attack_target : invalid_entity_id;
                    row.moving = unit.motion && unit.motion->kind != MotionKind::none;
                    row.arriving = staging_->arrivals->contains(unit.state.entity_id);
                    if (const auto* profile = unit.durability ? impl_->health_profile(unit) : nullptr;
                        profile && profile->damage_tracking)
                        row.time_till_dead = time_to_death(*profile, *unit.durability);
                    if (const auto box = impl_->collection_member(unit); !box)
                        throw std::runtime_error(box.error().message);
                    else if (box.value()) row.proximity_bounds = std::array{box.value()->bounds.min, box.value()->bounds.max};
                    const auto group = std::lower_bound(impl_->squadrons.begin(), impl_->squadrons.end(), unit.state.entity_id,
                        [](const Squadron& value, EntityId id) { return value.container < id; });
                    if (group != impl_->squadrons.end() && group->container == unit.state.entity_id) {
                        row.container = true;
                        row.members = group->members;
                    }
                    const auto parent = craft_squadron.find(unit.state.entity_id);
                    const auto formation_id = parent != craft_squadron.end() ? parent->second : unit.state.entity_id;
                    const auto formation = craft_prep_->minds->find(formation_id);
                    if (formation != craft_prep_->minds->end()) {
                        row.formation_target = formation->second.target;
                        row.formation_moving = formation->second.mode == SquadronMode::move;
                    }
                    if (unit.durability) {
                        row.has_durability = true;
                        row.hull = unit.durability->hull;
                        row.shields = unit.durability->shields;
                        row.damage_rate = unit.durability->damage_rate;
                    }
                    if (unit.abilities) row.slots = unit.abilities->slots;
                    if (parent != craft_squadron.end()) row.squadron_parent = parent->second;
                }
            });
            if (!staged_rows) return core::Result<void>::failure(staged_rows.error());
            auto produced = impl_->object_turn->run(executor, ObjectTurnView{tick, rows});
            if (!produced) return core::Result<void>::failure(produced.error());
            auto uses = std::move(produced).value();
            std::sort(uses.begin(), uses.end(), [](const ObjectTurnUse& left, const ObjectTurnUse& right) {
                return std::tie(left.unit, left.sequence) < std::tie(right.unit, right.sequence);
            });
            turn.reserve(uses.size());
            for (std::size_t index = 0; index < uses.size(); ++index) {
                const auto& use = uses[index];
                const auto label = "object-turn use (tick " + std::to_string(tick) + ", unit " + std::to_string(use.unit)
                    + ", sequence " + std::to_string(use.sequence) + ")";
                if (use.unit == invalid_entity_id || use.ability == AbilityKind::none
                    || static_cast<std::uint8_t>(use.ability) > static_cast<std::uint8_t>(AbilityKind::blast)
                    || static_cast<std::uint8_t>(use.action) < static_cast<std::uint8_t>(AbilityAction::activate)
                    || static_cast<std::uint8_t>(use.action) > static_cast<std::uint8_t>(AbilityAction::autofire_off)) {
                    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
                        label + " names no unit, ability or action"));
                }
                if (index != 0 && uses[index - 1].unit == use.unit && uses[index - 1].sequence == use.sequence) {
                    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::order,
                        label + " repeats the previous (unit, sequence)"));
                }
                turn.push_back(ObjectTurnRecord{tick, use});
            }
        }
    }
    for (auto iterator = first; iterator != last; ++iterator) turn.push_back(iterator->second);
    if (impl_->executed.size() + impl_->object_turns_executed.size() + object_turn_log_.size() + turn.size() > max_commands) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            "tick " + std::to_string(tick) + ": session command count limit reached by object-turn uses"));
    }
    for (const auto& record : turn) {
        auto reason = apply_object_turn_use(record.use, replans);
        if (!reason) return core::Result<void>::failure(reason.error());
        object_turn_outcomes_.push_back(ObjectTurnOutcome{record.use.unit, record.use.sequence, record.use.ability,
            record.use.action, reason.value()});
        object_turn_log_.push_back(record);
    }
    return core::Result<void>::success();
}

// One use at the seam, on the staged unit, through the activation paths the ability commands use. It
// refuses (with the command path's reason) what the commands would refuse; only a broken invariant fails.
core::Result<RejectReason> session_detail::Tick::apply_object_turn_use(const ObjectTurnUse& use, std::vector<EntityId>& replans) {
    using Result = core::Result<RejectReason>;
    auto& moving = gather_.value().moving.value();
    LiveUnit* unit = live_unit(moving, use.unit);
    if (unit == nullptr) return Result::success(RejectReason::unit_not_live);
    const auto original_order = unit->state.order;
    const auto release_cloak_team = [&](LiveUnit& container) {
        auto& team_minds = *craft_prep_->minds;
        const auto found = team_minds.find(container.state.entity_id);
        if (found == team_minds.end()) return;
        auto& team = team_minds.at(container.state.entity_id);
        const auto* escort = live_unit(moving, team.escorted);
        const bool preserve_escort = team.mode == SquadronMode::escort && escort != nullptr
            && !players_hostile(impl_->snapshot_players, container.state.owner, escort->state.owner);
        if (!preserve_escort && (team.target != invalid_entity_id || team.escorted != invalid_entity_id
            || (team.formation && team.formation->attack_target != invalid_entity_id))) {
            auto destination = container.state.position;
            if (team.attack_path && !team.attack_path->stack.empty() && !team.attack_path->stack.back().nodes.empty())
                destination = team.attack_path->stack.back().nodes.back();
            apply_squadron_order(team, MovePayload{destination}, container.state.position, impl_->motion.squadrons);
            container.state.order = order_for(MovePayload{destination}, tick);
        }
        if (preserve_escort) {
            team.attack_path.reset();
            if (team.formation) {
                auto& held = *team.formation;
                if (held.attack_override) held.complete = held.has_reached_done;
                held.attack_override = false;
                held.attack_target = invalid_entity_id;
                held.attack_hardpoint = attack_hull;
            }
            container.state.order = original_order;
        } else if (container.state.order.target != invalid_entity_id) container.state.order = {};
        team.target = invalid_entity_id; team.target_hardpoint = attack_hull; team.approach = false;
        if (container.combat) { container.combat->attack_target = invalid_entity_id; container.combat->direct = false; }
    };
    // AB-15: a container is one command. Validate all ordinary holders before
    // changing any slot, including a conflict with an earlier use in this turn.
    const auto group = std::find_if(impl_->squadrons.begin(), impl_->squadrons.end(),
        [&](const Squadron& value) { return value.container == use.unit; });
    if (group != impl_->squadrons.end() && !(group->members.size() == 1 && group->members.front() == use.unit)
        && use.ability != AbilityKind::buzz_droids && use.ability != AbilityKind::ion_cannon_shot
        && use.ability != AbilityKind::energy_weapon && use.ability != AbilityKind::tractor_beam
        && use.ability != AbilityKind::leech_shields) {
        std::vector<EntityId> holders;
        for (const auto member : group->members) {
            const auto* live = live_unit(moving, member);
            const auto* profile = live && live->abilities ? impl_->abilities.find(live->state.type_id) : nullptr;
            const auto index = profile ? ability_slot(*profile, use.ability) : std::nullopt;
            if (!index) continue;
            const auto& ability = profile->abilities[*index];
            const auto& slot = live->abilities->slots[*index];
            if (use.action == AbilityAction::activate
                && (slot.active || !ability_ready(ability, slot, impl_->ability_gate(*live, tick), tick)))
                return Result::success(RejectReason::ability_unavailable);
            if ((use.action == AbilityAction::autofire_on || use.action == AbilityAction::autofire_off)
                && !ability.supports_autofire) return Result::success(RejectReason::ability_unavailable);
            if (!object_turn_supports(use.ability)) return Result::success(RejectReason::ability_unavailable);
            if (use.action == AbilityAction::activate && (use.ability == AbilityKind::cluster_bomb
                || use.ability == AbilityKind::harmonic_bomb || use.ability == AbilityKind::weaken_enemy)) {
                if (!ability.spawned || !impl_->durability.damage) return Result::success(RejectReason::ability_unavailable);
                if (use.ability == AbilityKind::cluster_bomb && use.target != invalid_entity_id && use.target != use.unit)
                    return Result::success(RejectReason::ability_unavailable);
                if ((use.ability == AbilityKind::weaken_enemy) && !use.position)
                    return Result::success(RejectReason::ability_unavailable);
                const auto position = use.position && *use.position != math::Vec3{} ? *use.position : live->state.position;
                if (use.ability == AbilityKind::weaken_enemy && ability.spawned->reach.raw() > 0
                    && !within_range(live->state.position, position, ability.spawned->reach, RangeMetric::planar))
                    return Result::success(RejectReason::ability_unavailable);
            }
            holders.push_back(member);
            if (use.ability == AbilityKind::replenish_wingmen) break; // WHE-63: the leader owns replenishment
        }
        if (holders.empty()) return Result::success(RejectReason::ability_unavailable);
        for (const auto member : holders) {
            auto child = use; child.unit = member;
            if (child.ability == AbilityKind::cluster_bomb && child.target == use.unit) child.target = member;
            const auto applied = apply_object_turn_use(child, replans);
            if (!applied || applied.value() != RejectReason::none) return applied;
        }
        // WST-03: only the originating container request releases the shared team attack.
        if (use.ability == AbilityKind::stealth && use.action == AbilityAction::activate) release_cloak_team(*unit);
        return Result::success(RejectReason::none);
    }
    const auto* profile = unit->abilities ? impl_->abilities.find(unit->state.type_id) : nullptr;
    const auto index = profile ? ability_slot(*profile, use.ability) : std::nullopt;
    if (!index || !object_turn_supports(use.ability)) return Result::success(RejectReason::ability_unavailable);
    const auto& ability = profile->abilities[*index];
    auto& slot = unit->abilities->slots[*index];
    const auto speed_changed = [&] {
        if (unit->motion && unit->motion->kind == MotionKind::path && !unit->formation) replans.push_back(unit->state.entity_id);
    };
    auto& minds = *craft_prep_->minds;
    auto& crafts = *staging_->crafts;
    const auto clear_target = [&](LiveUnit& live) {
        if (live.combat) {
            live.combat->attack_target = invalid_entity_id;
            live.combat->attack_hardpoint = no_hardpoint;
            live.combat->direct = false;
        }
        if (live.state.order.target != invalid_entity_id) live.state.order = {};
        if (crafts.find(live.state.entity_id) != crafts.end()) {
            crafts.at(live.state.entity_id).chase = invalid_entity_id;
            crafts.at(live.state.entity_id).chase_until = 0;
        }
    };
    const auto stop = [&](LiveUnit& live) -> core::Result<void> {
        live.formation.reset();
        live.state.order = order_for(StopPayload{}, tick);
        if (minds.find(live.state.entity_id) != minds.end())
            apply_squadron_order(minds.at(live.state.entity_id), StopPayload{}, live.state.position, impl_->motion.squadrons);
        else if (live.motion) {
            if (auto stopped = impl_->plan_order(live, StopPayload{}, tick + 1); !stopped) return stopped;
            replans.push_back(live.state.entity_id);
        }
        return core::Result<void>::success();
    };
    const auto behaviour_container = [&]() -> LiveUnit* {
        const auto member = craft_prep_->craft_squadron->find(use.unit);
        return member != craft_prep_->craft_squadron->end() ? live_unit(moving, member->second) : unit;
    };
    if (use.ability == AbilityKind::energy_weapon || use.ability == AbilityKind::tractor_beam || use.ability == AbilityKind::leech_shields) {
        const auto* nested = impl_->beam_profile(*unit, use.ability);
        if (!nested) return Result::success(RejectReason::ability_unavailable);
        const auto previous = slot.target;
        if (use.action == AbilityAction::activate) {
            if (!impl_->beam_usable(*unit, use.ability, tick)) return Result::success(RejectReason::ability_unavailable);
            auto* target = live_unit(moving, use.target);
            if (!target) return Result::success(RejectReason::target_not_live);
            if (!impl_->beam_target_valid(*unit, *target, *nested) || !impl_->beam_in_range(*unit, *target, *nested)
                || (slot.active && use.ability == AbilityKind::energy_weapon)) return Result::success(RejectReason::ability_unavailable);
            if (slot.active) { impl_->release_beam(*unit, use.ability, tick); slot.ready_tick = tick; }
            static_cast<void>(impl_->lock_beam(*unit, use.target, use.ability, tick));
            if (use.ability == AbilityKind::energy_weapon) slot.target_hardpoint = impl_->energy_hardpoint(*unit, *target);
            if (use.ability == AbilityKind::tractor_beam) { target->formation.reset(); impl_->register_tractor(*unit, target); }
        } else if (use.action == AbilityAction::deactivate) {
            impl_->release_beam(*unit, use.ability, tick);
            if (use.ability == AbilityKind::tractor_beam) impl_->register_tractor(*unit, nullptr);
        } else {
            if (!ability.supports_autofire) return Result::success(RejectReason::ability_unavailable);
            slot.autofire = use.action == AbilityAction::autofire_on;
        }
        if (use.ability == AbilityKind::leech_shields) impl_->concentrate_changed_targets.insert(use.unit);
        if (use.ability == AbilityKind::tractor_beam || use.ability == AbilityKind::leech_shields) {
            if (auto refreshed = impl_->refresh_concentrate_bonuses(moving, impl_->command_ledger, *staging_->ledgers, executor); !refreshed)
                return Result::failure(refreshed.error());
            if (use.ability == AbilityKind::tractor_beam) for (const auto id : {previous, slot.target}) {
                if (const auto* target = live_unit(moving,id); target && target->motion && target->motion->kind == MotionKind::path)
                    replans.push_back(id);
            }
        }
        impl_->refresh_damage_modes(*unit);
        speed_changed();
        return Result::success(RejectReason::none);
    }
    switch (use.action) {
    case AbilityAction::autofire_on:
    case AbilityAction::autofire_off:
        if (!ability.supports_autofire) return Result::success(RejectReason::ability_unavailable);
        slot.autofire = use.action == AbilityAction::autofire_on;
        return Result::success(RejectReason::none);
    case AbilityAction::deactivate: {
        if (use.ability == AbilityKind::cluster_bomb || use.ability == AbilityKind::harmonic_bomb
            || use.ability == AbilityKind::weaken_enemy)
            return Result::success(RejectReason::none);
        const auto switched = deactivate_ability(ability, slot, tick);
        if (switched.changed && use.ability == AbilityKind::sensor_jamming) register_jammer(unit->state.entity_id, false);
        if (switched.changed && use.ability == AbilityKind::self_destruct) Impl::self_destruct_switched(*unit);
        if (switched.changed && use.ability == AbilityKind::stealth) Impl::clear_stealth(*unit);
        if (switched.changed && (use.ability == AbilityKind::hunt || use.ability == AbilityKind::lure)) {
            if (use.ability == AbilityKind::hunt) clear_target(*unit);
            if (auto* container = behaviour_container()) if (auto stopped = stop(*container); !stopped) return Result::failure(stopped.error());
        }
        if (switched.speed) speed_changed();
        if (switched.changed) impl_->refresh_damage_modes(*unit); // WHE-51
        return Result::success(RejectReason::none);
    }
    case AbilityAction::activate:
        break;
    }
    if (slot.active || !ability_ready(ability, slot, impl_->ability_gate(*unit, tick), tick))
        return Result::success(RejectReason::ability_unavailable);
    if (use.ability == AbilityKind::replenish_wingmen) {
        if (!impl_->motion.squadrons.find_squadron(ability.replenish_team)) return Result::success(RejectReason::ability_unavailable);
        std::vector<LiveUnit> members{*unit};
        const auto parent_group = std::find_if(impl_->squadrons.begin(), impl_->squadrons.end(), [&](const Squadron& value) {
            return std::find(value.members.begin(), value.members.end(), use.unit) != value.members.end();
        });
        if (parent_group != impl_->squadrons.end()) for (const auto id : parent_group->members)
            if (id != use.unit) if (const auto* member = live_unit(moving, id)) members.push_back(*member);
        std::sort(members.begin(), members.end(), [](const auto& left, const auto& right) { return left.state.entity_id < right.state.entity_id; });
        UnitStage selected(std::move(members));
        const auto first_added = *staging_->next_id;
        Impl::EconomyStage stage{*staging_->ledgers, *staging_->arrivals, *staging_->shares, crafts, minds,
            *staging_->arrived_squadrons, staging_->born_spawners, *staging_->earners, *staging_->next_id,
            impl_->pad_stage.values, impl_->construction_stage.values, *staging_->pad_requests,
            impl_->pad_stage, impl_->construction_stage, pending_economy_cues_, &*gather_->reinforcement_work};
        auto filled = impl_->replenish_wingmen(use.unit, ability.replenish_team, selected, stage, tick);
        if (!filled) return Result::failure(filled.error());
        if (filled.value()) {
            slot.active = false; slot.started_tick = tick; slot.ready_tick = tick + ability.recharge_frames;
            for (auto [id, born] : selected) if (id >= first_added) object_turn_births_.push_back(std::move(born));
            for (const auto& team : *staging_->arrived_squadrons)
                if (std::find(team.members.begin(), team.members.end(), use.unit) != team.members.end())
                    for (const auto member : team.members) (*craft_prep_->craft_squadron)[member] = team.container;
        }
        return Result::success(RejectReason::none);
    }
    if (use.ability == AbilityKind::cluster_bomb || use.ability == AbilityKind::harmonic_bomb
        || use.ability == AbilityKind::weaken_enemy) {
        if (!ability.spawned || !impl_->durability.damage
            || (use.ability == AbilityKind::cluster_bomb && use.target != invalid_entity_id && use.target != use.unit)
            || ((use.ability == AbilityKind::weaken_enemy) && !use.position))
            return Result::success(RejectReason::ability_unavailable);
        const auto position = use.position && *use.position != math::Vec3{} ? *use.position : unit->state.position;
        if (use.ability == AbilityKind::weaken_enemy && ability.spawned->reach.raw() > 0
            && !within_range(unit->state.position, position, ability.spawned->reach, RangeMetric::planar))
            return Result::success(RejectReason::ability_unavailable);
        spawn_instant_ability(*unit, use.ability, *ability.spawned, position);
        slot.started_tick = tick;
        slot.ready_tick = tick + ability.recharge_frames;
        return Result::success(RejectReason::none);
    }
    const auto switched = activate_ability(ability, slot, impl_->ability_gate(*unit, tick), tick);
    if (!switched.changed) return Result::success(RejectReason::ability_unavailable);
    if (use.ability == AbilityKind::sensor_jamming) register_jammer(unit->state.entity_id, true);
    if (use.ability == AbilityKind::defend) impl_->defend_switched_on(*unit, tick);
    if (use.ability == AbilityKind::self_destruct) Impl::self_destruct_switched(*unit);
    if (use.ability == AbilityKind::stealth) {
        impl_->stealth_switched_on(*unit, tick, impl_->stealth_duration_factor(unit->state.owner, *staging_->ledgers));
        if (crafts.find(use.unit) != crafts.end()) { crafts.at(use.unit).chase = invalid_entity_id; crafts.at(use.unit).chase_until = 0; }
        if (group != impl_->squadrons.end()) release_cloak_team(*unit); // singleton self-container
    }
    if (use.ability == AbilityKind::hunt || use.ability == AbilityKind::lure) {
        clear_target(*unit);
        if (auto* container = behaviour_container()) {
            clear_target(*container);
            if (minds.find(container->state.entity_id) != minds.end()) {
                auto& mind = minds.at(container->state.entity_id);
                mind.target = invalid_entity_id; mind.target_hardpoint = attack_hull; mind.approach = false;
            }
            if (use.ability == AbilityKind::lure) if (auto stopped = stop(*container); !stopped) return Result::failure(stopped.error());
        }
    }
    if (use.ability == AbilityKind::interdict || use.ability == AbilityKind::missile_shield)
        if (auto stopped = stop(*unit); !stopped) return Result::failure(stopped.error());
    if (switched.speed) speed_changed();
    impl_->refresh_damage_modes(*unit); // WHE-51
    return Result::success(RejectReason::none);
}

// Called from the tick's commit, with the other recorded commands: a failed tick leaves both lists alone.
void session_detail::Tick::commit_object_turn() {
    impl_->object_turn_pending.erase(impl_->object_turn_pending.begin(),
        impl_->object_turn_pending.lower_bound(std::tuple{tick + 1, EntityId{}, std::uint64_t{}}));
    impl_->object_turns_executed.insert(impl_->object_turns_executed.end(),
        std::make_move_iterator(object_turn_log_.begin()), std::make_move_iterator(object_turn_log_.end()));
    object_turn_log_.clear();
}

} // namespace eawr::sim::tactical
