#include "session_cmd.hpp"
#include "session_services.hpp"
#include "tactical_internal.hpp"

#include "eawr/core/text.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace eawr::sim::tactical {

const AbilitySlot* TacticalSession::Impl::lucky_slot(const LiveUnit& unit) const {
    const auto* profile = unit.abilities ? abilities.find(unit.state.type_id) : nullptr;
    const auto slot = profile ? ability_slot(*profile, AbilityKind::lucky_shot) : std::nullopt;
    return slot ? &unit.abilities->slots[*slot] : nullptr;
}

AbilitySlot* TacticalSession::Impl::lucky_slot(LiveUnit& unit) const {
    return const_cast<AbilitySlot*>(lucky_slot(static_cast<const LiveUnit&>(unit)));
}

const SpecialAbilityProfile* TacticalSession::Impl::lucky_profile(const LiveUnit& unit) const {
    const auto* profile = unit.abilities ? abilities.find(unit.state.type_id) : nullptr;
    const auto slot = profile ? ability_slot(*profile, AbilityKind::lucky_shot) : std::nullopt;
    if (!slot) return nullptr;
    const auto& name = profile->abilities[*slot].gui_activated_ability_name;
    const auto found = std::find_if(profile->special.begin(), profile->special.end(), [&](const auto& nested) {
        return nested.kind == SpecialAbilityKind::lucky_shot && core::iequals(nested.name, name);
    });
    return found == profile->special.end() ? nullptr : &*found;
}

bool TacticalSession::Impl::lucky_target_valid(const LiveUnit& source, const LiveUnit& target) const {
    const auto* nested = lucky_profile(source);
    const auto* owner = abilities.find(source.state.type_id);
    const auto* profile = combat.find_target_type(target.state.type_id);
    if (!nested || !owner || !profile || !target.durability || target.durability->hull.raw() <= 0
        || (target.nebula && target.nebula->present) || (source.nebula && source.nebula->present)
        || !players_hostile(snapshot_players, source.state.owner, target.state.owner)
        || !special_type_matches(nested->filter, target.state.type_id, profile->category_bits)
        || !named_ability_enabled(named_ability_locks, source.state.owner, source.state.type_id, *nested)) return false;
    const auto index = static_cast<std::size_t>(nested - owner->special.data());
    return !source.abilities->special.service_cancelled && index < source.abilities->special.slots.size()
        && !source.abilities->special.slots[index].cancelled
        && (nested->style == SpecialActivationStyle::user_input || nested->style == SpecialActivationStyle::space_automatic);
}

void TacticalSession::Impl::end_lucky_shot(LiveUnit& unit, const std::uint64_t tick) const {
    auto* slot = lucky_slot(unit);
    if (!slot || (!slot->active && !slot->lucky_spent)) return;
    const auto& profile = *abilities.find(unit.state.type_id);
    if (slot->lucky_spent) slot->ready_tick = tick + profile.abilities[*ability_slot(profile, AbilityKind::lucky_shot)].recharge_frames;
    slot->active = false;
    slot->lucky_spent = false;
    slot->target = invalid_entity_id;
    slot->target_hardpoint = no_hardpoint;
}

void TacticalSession::Impl::lock_lucky_shot(LiveUnit& unit, const EntityId target,
    const std::uint32_t hardpoint, const std::uint64_t tick) const {
    auto* slot = lucky_slot(unit);
    if (!slot) return;
    const bool starting = !slot->active;
    slot->active = true;
    slot->lucky_spent = false;
    if (starting) slot->started_tick = tick;
    slot->expires_tick = 0;
    slot->target = target;
    slot->target_hardpoint = hardpoint;
    unit.state.order = Order{.kind = OrderKind::attack, .issued_tick = tick, .destination = {},
        .target = target, .hardpoint = hardpoint, .through_hazards = false};
    if (unit.combat) {
        unit.combat->attack_target = target;
        unit.combat->attack_hardpoint = hardpoint;
        unit.combat->direct = true;
        if (starting) reset_lucky_weapon(unit);
    }
}

bool TacticalSession::Impl::lucky_in_range(const LiveUnit& source, const LiveUnit& target) const {
    auto maximum = attack_distance(source).value_or(math::Fixed{});
    if (const auto* squadron = motion.squadrons.find_squadron(source.state.type_id)) {
        for (const auto member : squadron->members) {
            const auto* craft = combat.find(member);
            if (craft && craft->max_attack_distance) maximum = std::max(maximum, *craft->max_attack_distance);
        }
    }
    return maximum.raw() > 0 && within_range(source.state.position, target.state.position, maximum, RangeMetric::planar);
}

void TacticalSession::Impl::reset_lucky_weapon(LiveUnit& unit) const {
    const auto* profile = unit.combat ? combat.find(unit.state.type_id) : nullptr;
    if (!profile) return;
    for (std::size_t index = 0; index < profile->weapons.size(); ++index) {
        if (profile->weapons[index].hardpoint != object_weapon) continue;
        unit.combat->weapons[index].countdown = 0;
        unit.combat->weapons[index].pulses_left = profile->weapons[index].pulse_count;
    }
}

core::Result<RejectReason> session_detail::Tick::CommandHandlers::apply_lucky_shot(const EntityId unit_id,
    const AbilityPayload& payload, const std::uint64_t& command_tick,
    detail::MapStage<SquadronState>& minds, UnitStage& staged) const {
    using Result = core::Result<RejectReason>;
    auto& source = staged.at(unit_id);
    auto* slot = phase.impl_->lucky_slot(source);
    if (!slot) return Result::success(RejectReason::ability_unavailable);
    const auto& profile = *phase.impl_->abilities.find(source.state.type_id);
    const auto& ability = profile.abilities[*ability_slot(profile, AbilityKind::lucky_shot)];
    if (payload.action == AbilityAction::autofire_on || payload.action == AbilityAction::autofire_off) {
        if (!ability.supports_autofire) return Result::success(RejectReason::ability_unavailable);
        slot->autofire = payload.action == AbilityAction::autofire_on;
        slot->lucky_autofire_set = true;
    } else if (payload.action == AbilityAction::deactivate) {
        phase.impl_->end_lucky_shot(source, command_tick);
    } else {
        if (slot->active || !ability_ready(ability, *slot, phase.impl_->ability_gate(source, command_tick), command_tick))
            return Result::success(RejectReason::ability_unavailable);
        const auto target = staged.find(payload.target);
        if (target == staged.end()) return Result::success(RejectReason::target_not_live);
        if (!phase.impl_->lucky_target_valid(source, target->second)) return Result::success(RejectReason::ability_unavailable);
        const auto* health = phase.impl_->health_profile(target->second);
        const auto hardpoint = health && payload.target_hardpoint != no_hardpoint
            && damage_target_valid(*health, payload.target_hardpoint) ? payload.target_hardpoint : no_hardpoint;
        phase.impl_->lock_lucky_shot(source, payload.target, hardpoint, command_tick);
        if (minds.find(unit_id) != minds.end()) {
            apply_squadron_order(minds.at(unit_id), AttackPayload{payload.target, hardpoint},
                source.state.position, phase.impl_->motion.squadrons);
            // The command commits the holder and its members together, as the ion team adapter does.
            for (const auto member : minds.at(unit_id).roster) {
                if (const auto craft = staged.find(member); craft != staged.end()) phase.impl_->reset_lucky_weapon(craft->second);
            }
        }
    }
    return Result::success(RejectReason::none);
}

core::Result<void> session_detail::Tick::lucky_shots() {
    if (std::none_of(impl_->abilities.profiles.begin(), impl_->abilities.profiles.end(), [](const auto& profile) {
        return ability_slot(profile, AbilityKind::lucky_shot).has_value();
    })) return core::Result<void>::success();
    auto& moving = gather_->moving.value();
    auto& minds = craft_prep_->minds.value();
    struct Action { std::size_t source{}; EntityId target{}; std::uint32_t hardpoint{no_hardpoint}; bool release{}; };
    std::vector<std::optional<Action>> actions(moving.size());
    // WLK-01..04: immutable input, disjoint work slots, ordered lock/order commits (ADR-009).
    const auto prepared = executor.execute_phase("lucky-shot-locks", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, moving.size());
        for (auto index = range.begin; index < range.end; ++index) {
            const auto& unit = moving[index];
            const auto* slot = impl_->lucky_slot(unit);
            const auto* nested = impl_->lucky_profile(unit);
            if (!slot || !nested) continue;
            const auto mind = minds.find(unit.state.entity_id);
            const auto attack = mind != minds.end() ? mind->second.target
                : unit.combat ? unit.combat->attack_target : invalid_entity_id;
            if (slot->active || slot->lucky_spent) {
                const auto* target = live_unit(moving, slot->target);
                if (slot->lucky_spent || !target || !impl_->lucky_target_valid(unit, *target))
                    actions[index] = Action{index, {}, no_hardpoint, true};
                else if (attack != slot->target) actions[index] = Action{index, slot->target, slot->target_hardpoint, false};
                continue;
            }
            // WLK-05/WAB-36: activation joins the shared authored-order autofire pass.
        }
    });
    if (!prepared) return prepared;
    for (const auto& action : actions) {
        if (!action) continue;
        auto& unit = moving[action->source];
        if (action->release) { impl_->end_lucky_shot(unit, tick); continue; }
        const bool starting = !impl_->lucky_slot(unit)->active;
        impl_->lock_lucky_shot(unit, action->target, action->hardpoint, tick);
        if (minds.find(unit.state.entity_id) != minds.end()) {
            apply_squadron_order(minds.at(unit.state.entity_id), AttackPayload{action->target, action->hardpoint},
                unit.state.position, impl_->motion.squadrons);
            // Sparse ordered cohort commit; all admission work above ran in partitions.
            if (starting) for (const auto member : minds.at(unit.state.entity_id).roster) {
                if (auto* craft = live_unit(moving, member)) impl_->reset_lucky_weapon(*craft);
            }
        }
    }
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
