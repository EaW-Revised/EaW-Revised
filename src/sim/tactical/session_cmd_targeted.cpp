#include "eawr/core/load_profile.hpp"
#include "eawr/core/tick_profile.hpp"
#include "eawr/sim/tactical/session.hpp"

#include "eawr/sim/tactical/formation.hpp"
#include "eawr/sim/tactical/pathfind.hpp"
#include "eawr/sim/math/trig.hpp"

#include "../math/wide.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "blast_internal.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "orders_internal.hpp"
#include "staging.hpp"
#include "session_cmd.hpp"
#include "session_services.hpp"
#include "tactical_internal.hpp"

#include "../../../third_party/entt/single_include/entt/entt.hpp"

#include <algorithm>
#include <array>
#include "diagnostic_timer.hpp"
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <deque>
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

core::Result<RejectReason> session_detail::Tick::CommandHandlers::apply_area_ability(const EntityId unit_id, const AreaAbilityPayload& payload) const {
    const auto command_tick = phase.tick;
    auto& next_id = phase.staging_.value().next_id.value();
    auto& staged = phase.tracked_.value().staged.value();
    auto& before = staged.at(unit_id);
    const auto* profile = phase.impl_->abilities.find(before.state.type_id);
    const auto index = profile != nullptr ? ability_slot(*profile, payload.ability) : std::nullopt;
    if (!index || !before.abilities || !before.combat || !before.motion) {
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    }
    const auto& ability = profile->abilities[*index];
    const auto& slot = before.abilities->slots[*index];
    if (slot.active) return core::Result<RejectReason>::success(RejectReason::none);
    if (!ability_ready(ability, slot, phase.impl_->ability_gate(before, command_tick), command_tick) || next_id == invalid_entity_id
        || phase.impl_->combat.find(ability.barrage_target_type) == nullptr) {
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    }
    const auto enemy = std::find_if(phase.impl_->setup.players.begin(), phase.impl_->setup.players.end(), [&](const Player& player) {
        return players_hostile(phase.impl_->snapshot_players, before.state.owner, player.player_id);
    });
    if (enemy == phase.impl_->setup.players.end()) return core::Result<RejectReason>::success(RejectReason::target_not_hostile);
    const auto own = std::lower_bound(phase.impl_->setup.players.begin(), phase.impl_->setup.players.end(), before.state.owner,
        [](const Player& player, const PlayerId id) { return player.player_id < id; });
    const auto player_index = static_cast<std::size_t>(own - phase.impl_->setup.players.begin());
    bool visible = false;
    if (phase.impl_->fog) visible = phase.impl_->fog->revealed(player_index, payload.point);
    else {
        // Point visibility uses the last published revealers; no serial world rebuild per click.
        const auto published = phase.impl_->current_snapshot;
        const auto observers = published->instances();
        std::array<std::uint8_t, tick_partition_count> revealed{};
        const auto checked = phase.executor.execute_phase("barrage-point-visibility", tick_partition_count,
            [&](const std::size_t partition) {
                const auto range = partition_range(partition, observers.size());
                for (auto index = range.begin; index < range.end; ++index) {
                    const auto& observer = observers[index];
                    if (observer.team != own->team_id || !observer.reveal_range) continue;
                    const math::Vec3 position{observer.fixed_transform.rows[0][3],
                        observer.fixed_transform.rows[1][3], observer.fixed_transform.rows[2][3]};
                    if (within_range(position, payload.point, *observer.reveal_range, RangeMetric::planar)) {
                        revealed[partition] = 1U;
                        break;
                    }
                }
            });
        if (!checked) return core::Result<RejectReason>::failure(checked.error());
        visible = std::any_of(revealed.begin(), revealed.end(), [](const std::uint8_t value) { return value != 0; });
    }
    if (!visible) return core::Result<RejectReason>::success(RejectReason::invalid_position);
    auto z = math::add(payload.point.z, ability.target_z_offset);
    if (!z) return core::Result<RejectReason>::success(RejectReason::invalid_position);
    LiveUnit proxy;
    proxy.state = UnitState{next_id, ability.barrage_target_type, enemy->player_id,
        {payload.point.x, payload.point.y, z.value()}, math::identity_quat(), {}};
    proxy.state.barrage_source = unit_id;
    const auto proxy_id = next_id++;
    staged.emplace(proxy_id, std::move(proxy));
    phase.track_victory_change(nullptr, &staged.at(proxy_id).state, false);
    auto& live = staged.at(unit_id); // insertion can move the vector
    auto& active = live.abilities->slots[*index];
    static_cast<void>(activate_ability(ability, active, phase.impl_->ability_gate(live, command_tick), command_tick));
    active.target = proxy_id;
    const CommandPayload attack = AttackPayload{proxy_id};
    live.state.order = order_for(attack, command_tick);
    live.combat->attack_target = proxy_id;
    live.combat->attack_hardpoint = no_hardpoint;
    live.combat->direct = true;
    const auto range = phase.impl_->approach_range(live, false);
    if (!within_range(live.state.position, staged.at(proxy_id).state.position, range, RangeMetric::planar)) {
        auto mapped = phase.impl_->approach_mapping(live, staged.at(proxy_id), range, command_tick + 1);
        if (!mapped) return core::Result<RejectReason>::failure(mapped.error());
        if (auto planned = plan_approach(unit_id, live, mapped.value()); !planned) {
            return core::Result<RejectReason>::failure(planned.error());
        }
    }
    return core::Result<RejectReason>::success(RejectReason::none);
}

core::Result<RejectReason> session_detail::Tick::CommandHandlers::apply_manual_target(const EntityId unit_id, const ManualTargetPayload& payload, const PlayerId issuer, const bool player_clock) const {
    const auto command_tick = phase.tick;
    auto& arrivals = phase.staging_.value().arrivals.value();
    auto& ledgers = phase.staging_.value().ledgers.value();
    const auto& world = phase.targets_.value().world.value();
    auto& staged = phase.tracked_.value().staged.value();
    const auto& view = world.value();
    const auto target = staged.find(payload.target);
    if (target == staged.end()) return core::Result<RejectReason>::success(RejectReason::target_not_live);
    auto& live = staged.at(unit_id);
    const auto* own_view = view.find(unit_id);
    const auto* target_view = view.find(payload.target);
    if (!live.combat || own_view == nullptr || target_view == nullptr || own_view->profile == nullptr) {
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    }
    const auto selected = std::find_if(own_view->profile->weapons.begin(), own_view->profile->weapons.end(),
        [&](const WeaponProfile& weapon) { return weapon.hardpoint == payload.hardpoint; });
    // WSW-07/40: installed manual special weapons require an enabling effect;
    // isolated gun-contract fixtures without modifier profiles retain their existing admission.
    if (player_clock && selected != own_view->profile->weapons.end() && selected->special
        && std::any_of(phase.impl_->economy.upgrades.begin(), phase.impl_->economy.upgrades.end(),
            [](const UpgradeProfile& upgrade) { return !upgrade.special_unlocks.empty(); })
        && !phase.impl_->special_weapons_enabled(issuer, ledgers, &staged, &phase.special_disabled_by_hardpoint_))
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    const auto clock = phase.manual_clocks_.find(issuer);
    if (player_clock && clock != phase.manual_clocks_.end() && manual_readiness(clock->second, command_tick).raw() < math::Fixed::scale) {
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    }
    // WSW-17/40: the same public frozen predicate backs the HUD, Lua and commands.
    // Copy only these two current staged objects, with the immutable visibility relation.
    // WWP-73: admission and the eventual shot use the same rolled weapon frame.
    const auto own_transform = unit_transform(live);
    if (!own_transform) return core::Result<RejectReason>::failure(own_transform.error());
    const auto enemy_transform = unit_transform(target->second);
    if (!enemy_transform) return core::Result<RejectReason>::failure(enemy_transform.error());
    auto own = phase.impl_->instance_for(live, own_transform.value(), command_tick, &phase.special_weapons_);
    auto enemy = phase.impl_->instance_for(target->second, enemy_transform.value(), command_tick, &phase.special_weapons_);
    enemy.visible_to = target_view->visible_to;
    if (const auto arrival = arrivals.find(unit_id); arrival != arrivals.end()) own.arrival = arrival->second.frame;
    auto admitted = manual_special_fire_admission(phase.impl_->snapshot_players, own, enemy, !player_clock, payload.hardpoint);
    if (!admitted) return core::Result<RejectReason>::failure(admitted.error());
    if (!admitted.value()) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    const auto weapon = std::find_if(own_view->profile->weapons.begin(), own_view->profile->weapons.end(),
        [&](const WeaponProfile& candidate) { return candidate.hardpoint == payload.hardpoint; });
    const auto index = static_cast<std::size_t>(weapon - own_view->profile->weapons.begin());
    auto& assignment = *live.combat->weapons[index].manual;
    assignment.target = payload.target;
    assignment.requesting_player = issuer;
    assignment.assigned_frame = command_tick;
    return core::Result<RejectReason>::success(RejectReason::none);
}

core::Result<void> session_detail::Tick::CommandHandlers::special_fire(const PlayerCommand& command, const SpecialFirePayload& payload, const PlayerId issuer) const {
    const auto command_tick = phase.tick;
    auto& ledgers = phase.staging_.value().ledgers.value();
    const auto& world = phase.targets_.value().world.value();
    auto& staged = phase.tracked_.value().staged.value();
    auto& events = phase.impacts_.value().events.value();
    const auto& view = world.value();
    const auto* special = &payload;
    const auto source_id = command.units.front();
    const auto source = staged.find(source_id);
    const auto target = staged.find(special->target);
    const auto player = phase.impl_->economy.player_at(issuer, command_tick);
    const auto* profile = source != staged.end() ? phase.impl_->combat.find(source->second.state.type_id) : nullptr;
    auto reason = RejectReason::none;
    const bool ai = special->admission != SpecialFireAdmission::player;
    bool dispatched = false, fired = false;
    std::uint32_t index = 0;
    if (ai && (!player || !player->ai)) reason = RejectReason::ability_unavailable;
    else if (source == staged.end()) reason = RejectReason::unit_not_live;
    else if (!profile || !phase.impl_->special_weapons_enabled(issuer, ledgers, &staged, &phase.special_disabled_by_hardpoint_)) reason = RejectReason::ability_unavailable;
    else if (!phase.impl_->allied(issuer, source->second.state.owner)) reason = RejectReason::unit_not_owned;
    else if (profile->planetary_weapon) {
        const auto state = phase.special_weapons_.find(source_id);
        index = profile->planetary_weapon->index;
        if (source->second.state.owner != issuer) reason = RejectReason::unit_not_owned;
        else if (state == phase.special_weapons_.end()) reason = RejectReason::ability_unavailable;
        else {
            auto& clock = state->second;
            // WSW-16: the player event admits at .99; the helper still needs full readiness.
            const auto duration = clock.recharge_until > clock.recharge_started
                ? clock.recharge_until - clock.recharge_started : 0;
            const auto elapsed = command_tick > clock.recharge_started ? command_tick - clock.recharge_started : 0;
            const bool outer_ready = command_tick >= clock.recharge_until || (!ai && duration && elapsed * 100 >= duration * 99);
            if (!outer_ready) reason = RejectReason::ability_unavailable;
            else {
                dispatched = true;
                if (command_tick < clock.recharge_until) reason = RejectReason::ability_unavailable;
                else if (target == staged.end()) reason = RejectReason::target_not_live;
                else {
                    const auto own_transform = unit_transform(source->second);
                    if (!own_transform) return core::Result<void>::failure(own_transform.error());
                    const auto enemy_transform = unit_transform(target->second);
                    if (!enemy_transform) return core::Result<void>::failure(enemy_transform.error());
                    const auto own = phase.impl_->instance_for(source->second, own_transform.value(), command_tick, &phase.special_weapons_);
                    const auto enemy = phase.impl_->instance_for(target->second, enemy_transform.value(), command_tick, &phase.special_weapons_);
                    reason = planetary_special_fire_target_admission(phase.impl_->snapshot_players, own, enemy);
                    if (reason == RejectReason::none && (!view.find(source_id) || !view.find(special->target)))
                        reason = RejectReason::ability_unavailable;
                }
                if (reason == RejectReason::none) {
                    // WSW-19/22: wrapper success starts clocks even if shared shot construction later rejects.
                    clock.recharge_started = command_tick;
                    clock.recharge_until = command_tick + profile->planetary_weapon->recharge_frames;
                    clock.next_pulse_frame = command_tick + profile->planetary_weapon->pulse_delay_frames;
                    clock.target = special->target;
                    clock.pulses_left = profile->weapons.front().pulse_count - 1;
                    phase.special_initial_.insert(source_id);
                    SpecialShot shot;
                    shot.source = *view.find(source_id);
                    shot.target = *view.find(special->target);
                    shot.source.position = source->second.state.position;
                    // WWP-73/74: pulse one shares the banked placement of later pulses.
                    const auto source_transform = unit_transform(source->second);
                    if (!source_transform) return core::Result<void>::failure(source_transform.error());
                    shot.source.transform = source_transform.value();
                    shot.target.position = target->second.state.position;
                    const auto target_transform = unit_transform(target->second);
                    if (!target_transform) return core::Result<void>::failure(target_transform.error());
                    shot.target.transform = target_transform.value();
                    shot.target_health = target->second.durability;
                    if (const auto* health = phase.impl_->health_profile(target->second)) shot.target_health_profile = *health;
                    phase.special_shots_.push_back(std::move(shot));
                    if (!clock.pulses_left) clock.target = invalid_entity_id;
                    fired = true;
                }
            }
        }
    } else if (!profile->star_base || special->admission == SpecialFireAdmission::ai_taskforce)
        reason = RejectReason::ability_unavailable;
    else {
        const auto weapon = std::find_if(profile->weapons.begin(), profile->weapons.end(),
            [](const WeaponProfile& entry) { return entry.requires_manual_target; });
        if (weapon == profile->weapons.end() || !source->second.combat) reason = RejectReason::ability_unavailable;
        else {
            const auto slot = static_cast<std::size_t>(weapon - profile->weapons.begin());
            if (slot >= source->second.combat->weapons.size()
                || (ai && source->second.combat->weapons[slot].countdown != 0)) reason = RejectReason::ability_unavailable;
            else {
                dispatched = true;
                auto assigned = apply_manual_target(source_id, {special->target, weapon->hardpoint}, issuer, !ai);
                if (!assigned) return core::Result<void>::failure(assigned.error());
                reason = assigned.value();
                fired = reason == RejectReason::none;
            }
        }
    }
    // WSW-09/10: player cleanup follows admitted dispatch, even a rejected helper;
    // AI cleanup follows success and keeps its entry-route owner selection.
    if ((!ai && dispatched) || (ai && fired)) {
        const auto owner = special->admission == SpecialFireAdmission::ai_object ? issuer
            : source->second.state.owner;
        if (auto* account = phase.impl_->ledger_of(ledgers, owner)) {
            std::vector<CompletedBuild> removed;
            std::erase_if(account->completed, [&](const CompletedBuild& held) {
                const auto* upgrade = phase.impl_->economy.upgrade(held.type);
                if (!upgrade || !upgrade->consume_after_special_fire) return false;
                removed.push_back(held);
                return true;
            });
            for (const auto& held : removed) phase.impl_->retire_income_modifier(*account, held);
        }
    }
    events.push_back(Event{command_tick, reason == RejectReason::none ? EventKind::order_accepted : EventKind::order_rejected,
        issuer, command.key.sequence, source_id, OrderKind::special_fire, reason});
    if (fired && profile->planetary_weapon)
        events.push_back(Event{command_tick, EventKind::special_weapon_fired, source->second.state.owner, index, source_id});
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
