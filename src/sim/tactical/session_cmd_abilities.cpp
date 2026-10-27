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

std::vector<LiveUnit*> session_detail::Tick::CommandHandlers::ordinary_ability_holders(const EntityId unit_id, const AbilityKind kind) const {
    auto& staged = phase.tracked_.value().staged.value();
    std::vector<LiveUnit*> holders;
    const auto add = [&](const EntityId id) {
        const auto found = staged.find(id);
        if (found == staged.end() || !found->second.abilities) return;
        const auto* profile = phase.impl_->abilities.find(found->second.state.type_id);
        if (profile && ability_slot(*profile, kind)) holders.push_back(&found->second);
    };
    const auto squadron = std::find_if(phase.impl_->squadrons.begin(), phase.impl_->squadrons.end(),
        [unit_id](const Squadron& entry) { return entry.container == unit_id; });
    if (squadron == phase.impl_->squadrons.end()) add(unit_id);
    else for (const auto member : squadron->members) add(member);
    return holders;
}

// An ability command on one listed unit (AB-10 to AB-15, AB-40): the unit itself, or for a
// squadron container its live craft that have the ability. Every holder must be ready and
// off for an activation to act (AB-15); otherwise nothing changes.
core::Result<RejectReason> session_detail::Tick::CommandHandlers::apply_blast(EntityId unit_id,
        const AbilityPayload& payload, const std::uint64_t& command_tick, UnitStage& staged) const {
    auto& source = staged.at(unit_id);
    const auto* profile = phase.impl_->abilities.find(source.state.type_id);
    const auto index = profile ? ability_slot(*profile, payload.ability) : std::nullopt;
    if (!index || !source.abilities) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    const auto& ability = profile->abilities[*index];
    const auto nested = std::find_if(profile->special.begin(), profile->special.end(), [&](const auto& special) {
        return special.kind == SpecialAbilityKind::blast && special.name == ability.gui_activated_ability_name;
    });
    if (nested == profile->special.end() || !ability.charged_blast)
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    if (payload.action == AbilityAction::deactivate) return core::Result<RejectReason>::success(RejectReason::none);
    auto& handler = source.abilities->special.slots[static_cast<std::size_t>(nested - profile->special.begin())];
    auto& slot = source.abilities->slots[*index];
    if (payload.action != AbilityAction::activate || nested->initially_enabled == false
        || source.abilities->special.service_cancelled || handler.cancelled
        || !ability_ready(ability, slot, phase.impl_->ability_gate(source, command_tick), command_tick))
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    if (!activate_ability(ability, slot, phase.impl_->ability_gate(source, command_tick), command_tick).changed)
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    // WHE-35, FC-62: instant initiation starts recharge independently of the handler budget.
    slot.active = false;
    source.abilities->charged_blast = ChargedBlastState{ability.charged_blast->charge_frames, 0, false};
    handler.enabled = true;
    handler.next_service_frame = command_tick + 1; // commands follow this tick's pre-fire poll
    return core::Result<RejectReason>::success(RejectReason::none);
    }

core::Result<RejectReason> session_detail::Tick::CommandHandlers::apply_corrupt_systems(EntityId unit_id,
        const AbilityPayload& payload, const std::uint64_t& command_tick,
        std::map<EntityId, ArrivalState>& arrivals, const std::uint64_t& frame, UnitStage& staged,
        const MotionTable& table) const {
    auto& source = staged.at(unit_id);
    const auto* profile = phase.impl_->abilities.find(source.state.type_id);
    const auto index = profile ? ability_slot(*profile, payload.ability) : std::nullopt;
    if (!index || !source.abilities) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    const auto& ability = profile->abilities[*index];
    const auto nested = std::find_if(profile->special.begin(), profile->special.end(), [&](const auto& special) {
        return special.kind == SpecialAbilityKind::corrupt_systems && special.name == ability.gui_activated_ability_name;
    });
    if (nested == profile->special.end()) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    auto& slot = source.abilities->slots[*index];
    if (payload.action == AbilityAction::deactivate) return core::Result<RejectReason>::success(RejectReason::none);
    if (payload.action != AbilityAction::activate || !payload.position
        || nested->initially_enabled == false || nested->style != SpecialActivationStyle::user_input
        || !ability_ready(ability, slot, phase.impl_->ability_gate(source, command_tick), command_tick))
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    const auto point = *payload.position;
    if (!within_range(source.state.position, point, nested->corrupt_max_range, RangeMetric::planar)
        || (nested->corrupt_min_range.raw() > 0 && within_range(source.state.position, point,
            nested->corrupt_min_range, RangeMetric::planar)))
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    // WHE-33: query existing enemy collidable trees, then stage eligibility in partitions.
    const auto radius = ability.effective_radius;
    const auto axis = [](const math::Fixed coordinate, const math::Fixed offset) {
        return math::Fixed::from_raw(coordinate.raw() + offset.raw());
    };
    const auto negative = math::Fixed::from_raw(-radius.raw());
    const detail::CullBox box{{axis(point.x, negative), axis(point.y, negative), math::Fixed::from_raw(std::numeric_limits<std::int64_t>::min())},
        {axis(point.x, radius), axis(point.y, radius), math::Fixed::from_raw(std::numeric_limits<std::int64_t>::max())}};
    std::vector<EntityId> candidates;
    for (const auto& player : phase.impl_->snapshot_players) {
        if (!players_hostile(phase.impl_->snapshot_players, source.state.owner, player.player_id)) continue;
        const auto found = phase.targets_->collection->collect(player.player_id, box);
        candidates.insert(candidates.end(), found.begin(), found.end());
    }
    if (!concentrate_new_bodies.empty()) {
        const auto added = SpaceIndex::build(concentrate_new_bodies);
        if (!added) return core::Result<RejectReason>::failure(added.error());
        std::vector<std::uint32_t> positions;
        added.value().box_positions(point, {radius, radius, math::Fixed::from_raw(max_motion_coordinate * math::Fixed::scale)}, positions);
        for (const auto position : positions) candidates.push_back(added.value().bodies()[position].entity_id);
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    std::vector<std::uint8_t> selected(candidates.size());
    std::vector<std::optional<core::Diagnostic>> recipient_errors(candidates.size());
    auto& corrupt_work = phase.gather_->tick_work.value();
    ++corrupt_work.corrupt_queries;
    corrupt_work.corrupt_candidates += candidates.size();
    const auto gathered = phase.executor.execute_phase("corrupt-recipients", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, candidates.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto found = staged.find(candidates[i]);
            if (found == staged.end() || candidates[i] == unit_id) continue;
            const auto& recipient = found->second;
            if (!players_hostile(phase.impl_->snapshot_players, source.state.owner, recipient.state.owner)) continue;
            const auto* combat = phase.impl_->combat.find(recipient.state.type_id);
            if (!combat || !combat->living_projectile_collision
                || (!phase.impl_->motion.find(recipient.state.type_id) && !phase.impl_->motion.squadrons.find_craft(recipient.state.type_id))
                || arrivals.contains(candidates[i]) || (recipient.durability && recipient.durability->hull.raw() <= 0)) continue;
            if (!within_range(point, recipient.state.position, radius, RangeMetric::planar)) {
                // WHE-69: outside centers may still overlap by their transformed collision box.
                detail::CombatUnit geometry;
                geometry.profile = combat;
                const auto transform = unit_transform(recipient);
                if (!transform) { recipient_errors[i] = transform.error(); continue; }
                geometry.transform = transform.value();
                const auto overlap = detail::collision_overlaps_sphere(geometry, point, radius);
                if (!overlap) { recipient_errors[i] = overlap.error(); continue; }
                if (!overlap.value()) continue;
            }
            selected[i] = 1;
        }
    });
    if (!gathered) return core::Result<RejectReason>::failure(gathered.error());
    for (const auto& error : recipient_errors) if (error) return core::Result<RejectReason>::failure(*error);
    for (std::size_t i = 0; i < candidates.size(); ++i) if (selected[i]) {
        ++corrupt_work.corrupt_recipients;
        staged.at(candidates[i]).corrupt = CorruptSystemsState{source.state.type_id, command_tick,
            command_tick + nested->corrupt_duration_frames, nested->corrupt_drain_per_frame, nested->corrupt_inaccuracy};
    }
    slot.started_tick = command_tick;
    slot.ready_tick = command_tick + ability.recharge_frames;
    slot.position = point;
    source.state.order = order_for(StopPayload{}, command_tick);
    if (source.motion) {
        if (auto stopped = phase.impl_->plan_order(source, StopPayload{}, frame); !stopped)
            return core::Result<RejectReason>::failure(stopped.error());
        if (const auto* footprint = table.avoidance ? tracked_footprint(table, source.state.type_id) : nullptr) {
            if (const auto layer = dynamic_layer_index(footprint->layer)) {
                if (auto submitted = submit(unit_id, source, *layer, std::nullopt); !submitted)
                    return core::Result<RejectReason>::failure(submitted.error());
            }
        }
    }
    return core::Result<RejectReason>::success(RejectReason::none);
    }

core::Result<RejectReason> session_detail::Tick::CommandHandlers::apply_replenish_wingmen(EntityId unit_id,
        const AbilityPayload& payload, const std::uint64_t& command_tick,
        TacticalSession::PlacementWork& reinforcement_work, detail::MapStage<SquadronState>& minds,
        detail::MapStage<CraftState>& crafts, std::map<EntityId, ArrivalState>& arrivals,
        std::vector<PlayerEconomy>& ledgers, std::vector<PlayerCommand>& pad_requests,
        std::map<EntityId, PopulationShare>& shares, std::vector<Squadron>& arrived_squadrons,
        std::vector<std::pair<EntityId, PlayerId>>& earners, EntityId& next_id, UnitStage& staged,
        const SquadronTable& squadron_table, std::map<EntityId, PadState>& pads,
        std::map<EntityId, ConstructionState>& construction) const {
    LiveUnit* source = &staged.at(unit_id);
    if (const auto group = std::find_if(phase.impl_->squadrons.begin(), phase.impl_->squadrons.end(),
            [unit_id](const Squadron& entry) { return entry.container == unit_id; }); group != phase.impl_->squadrons.end()) {
        source = nullptr;
        for (const auto member : group->members) {
            const auto found = staged.find(member);
            const auto* profile = found != staged.end() ? phase.impl_->abilities.find(found->second.state.type_id) : nullptr;
            if (profile && ability_slot(*profile, payload.ability)) { source = &found->second; break; }
        }
    }
    const auto* profile = source ? phase.impl_->abilities.find(source->state.type_id) : nullptr;
    const auto slot = profile ? ability_slot(*profile, payload.ability) : std::nullopt;
    if (!source || !source->abilities || !slot) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    const auto& ability = profile->abilities[*slot];
    auto& state = source->abilities->slots[*slot];
    if (payload.action == AbilityAction::autofire_on || payload.action == AbilityAction::autofire_off) {
        if (!ability.supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        state.autofire = payload.action == AbilityAction::autofire_on;
        return core::Result<RejectReason>::success(RejectReason::none);
    }
    if (payload.action != AbilityAction::activate || !ability_ready(ability, state, phase.impl_->ability_gate(*source, command_tick), command_tick)
        || !squadron_table.find_squadron(ability.replenish_team))
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    Impl::EconomyStage stage{ledgers, arrivals, shares, crafts, minds, arrived_squadrons,
        phase.staging_.value().born_spawners, earners, next_id,
        pads, construction, pad_requests, phase.impl_->pad_stage, phase.impl_->construction_stage,
        phase.pending_economy_cues_, &reinforcement_work};
    const auto leader_id = source->state.entity_id;
    const auto first_added = next_id;
    auto filled = phase.impl_->replenish_wingmen(leader_id, ability.replenish_team, staged, stage, command_tick);
    if (!filled) return core::Result<RejectReason>::failure(filled.error());
    phase.sync_radar_creations(staged, first_added, next_id);
    if (filled.value()) {
        auto& current = staged.at(leader_id).abilities->slots[*slot];
        current.active = false;
        current.started_tick = command_tick;
        current.ready_tick = command_tick + ability.recharge_frames;
        for (const auto& group : arrived_squadrons)
            if (std::find(group.members.begin(), group.members.end(), leader_id) != group.members.end())
                for (const auto member : group.members) phase.craft_prep_->craft_squadron.value()[member] = group.container;
    }
    return core::Result<RejectReason>::success(RejectReason::none);
    }

core::Result<RejectReason> session_detail::Tick::CommandHandlers::apply_spawned_ability(EntityId unit_id,
        const AbilityPayload& payload, const std::uint64_t& command_tick) const {
    // AB-15/WFP-02: ordinary spawned abilities act on the container's
    // eligible craft. Admit every holder before committing any spawn.
    const auto holders = ordinary_ability_holders(unit_id, payload.ability);
    if (holders.empty() || !phase.impl_->durability.damage)
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    for (const auto* holder : holders) {
        const auto& profile = *phase.impl_->abilities.find(holder->state.type_id);
        const auto slot = *ability_slot(profile, payload.ability);
        const auto& ability = profile.abilities[slot];
        if (!ability.spawned && !ability.unresolved_spawn) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        if (payload.action == AbilityAction::autofire_on || payload.action == AbilityAction::autofire_off) {
            if (!ability.supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        } else if (payload.action == AbilityAction::activate) {
            if (holder->abilities->slots[slot].active
                || !ability_ready(ability, holder->abilities->slots[slot], phase.impl_->ability_gate(*holder, command_tick), command_tick))
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            if (payload.ability == AbilityKind::cluster_bomb && payload.target != invalid_entity_id && payload.target != unit_id)
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            if ((payload.ability == AbilityKind::fow_reveal_ping || payload.ability == AbilityKind::weaken_enemy)
                && !payload.position) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            const auto position = payload.position && *payload.position != math::Vec3{}
                ? *payload.position : holder->state.position;
            if (payload.ability == AbilityKind::weaken_enemy && ability.spawned && ability.spawned->reach.raw() > 0
                && !within_range(holder->state.position, position, ability.spawned->reach, RangeMetric::planar))
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        }
    }
    if (payload.action == AbilityAction::deactivate) return core::Result<RejectReason>::success(RejectReason::none);
    for (auto* holder : holders) {
        const auto& profile = *phase.impl_->abilities.find(holder->state.type_id);
        const auto slot = *ability_slot(profile, payload.ability);
        const auto& ability = profile.abilities[slot];
        auto& state = holder->abilities->slots[slot];
        if (payload.action == AbilityAction::autofire_on || payload.action == AbilityAction::autofire_off) {
            // FS-29/FSC-46: only an admitted object-script turn observes this toggle.
            state.autofire = payload.action == AbilityAction::autofire_on;
            continue;
        }
        auto position = holder->state.position;
        if ((payload.ability == AbilityKind::fow_reveal_ping || payload.ability == AbilityKind::weaken_enemy)
            && *payload.position != math::Vec3{}) position = *payload.position;
        if (ability.spawned) phase.spawn_instant_ability(*holder, payload.ability, *ability.spawned, position);
        state.started_tick = command_tick;
        state.ready_tick = command_tick + ability.recharge_frames;
    }
    return core::Result<RejectReason>::success(RejectReason::none);
    }

core::Result<RejectReason> session_detail::Tick::CommandHandlers::apply_beam_ability(EntityId unit_id,
        const AbilityPayload& payload, const std::uint64_t& command_tick, std::vector<PlayerEconomy>& ledgers,
        UnitStage& staged) const {
    auto& source = staged.at(unit_id);
    const auto* nested = phase.impl_->beam_profile(source, payload.ability);
    if (!source.abilities || !nested) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    const auto& profile = *phase.impl_->abilities.find(source.state.type_id);
    const auto slot = *ability_slot(profile, payload.ability);
    auto& state = source.abilities->slots[slot];
    const auto& ability = profile.abilities[slot];
    if (payload.action == AbilityAction::autofire_on || payload.action == AbilityAction::autofire_off) {
        if (!ability.supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        state.autofire = payload.action == AbilityAction::autofire_on;
        return core::Result<RejectReason>::success(RejectReason::none);
    }
    const auto previous = state.target;
    if (payload.action == AbilityAction::deactivate) {
        phase.impl_->release_beam(source, payload.ability, command_tick);
        if (payload.ability == AbilityKind::tractor_beam) phase.impl_->register_tractor(source, nullptr);
    } else {
        if (!phase.impl_->beam_usable(source, payload.ability, command_tick))
            return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        const auto target = staged.find(payload.target);
        if (target == staged.end()) return core::Result<RejectReason>::success(RejectReason::target_not_live);
        if (!phase.impl_->beam_target_valid(source, target->second, *nested) || !phase.impl_->beam_in_range(source, target->second, *nested))
            return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        if (state.active && payload.ability == AbilityKind::energy_weapon)
            return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        // WHE-60: retarget unlocks the old recipient before installing the new source entry.
        if (state.active) {
            phase.impl_->release_beam(source, payload.ability, command_tick);
            state.ready_tick = command_tick;
        }
        static_cast<void>(phase.impl_->lock_beam(source, payload.target, payload.ability, command_tick));
        if (payload.ability == AbilityKind::energy_weapon) {
            state.target_hardpoint = payload.target_hardpoint != no_hardpoint
                    && phase.impl_->target_hardpoint_standing(target->second, payload.target_hardpoint)
                ? payload.target_hardpoint : phase.impl_->energy_hardpoint(source, target->second);
        }
        if (payload.ability == AbilityKind::tractor_beam) {
            target->second.formation.reset();
            phase.impl_->register_tractor(source, &target->second);
        }
    }
    if (payload.ability == AbilityKind::tractor_beam) {
        std::map<EntityId, TypeId> containers;
        for (const auto& account : ledgers) for (const auto& held : account.completed)
            if (const auto found = staged.find(held.station); found != staged.end())
                containers.emplace(held.station, found->second.state.type_id);
        std::vector<CombatBonuses> categories(phase.impl_->bonus_categories.size());
        for (const auto id : {previous, state.target}) if (const auto target = staged.find(id); target != staged.end()) {
            if (auto applied = phase.impl_->apply_bonuses(target->second, phase.impl_->command_bonuses_for(target->second,
                phase.impl_->command_ledger, ledgers, containers, categories, &staged), Impl::BonusAdjustment::loss); !applied)
                return core::Result<RejectReason>::failure(applied.error());
            if (auto planned = replan(id); !planned) return core::Result<RejectReason>::failure(planned.error());
        }
    }
    if (payload.ability == AbilityKind::leech_shields) {
        std::map<EntityId, TypeId> containers;
        for (const auto& account : ledgers) for (const auto& held : account.completed)
            if (const auto found = staged.find(held.station); found != staged.end())
                containers.emplace(held.station, found->second.state.type_id);
        std::vector<CombatBonuses> categories(phase.impl_->bonus_categories.size());
        if (auto applied = phase.impl_->apply_bonuses(source, phase.impl_->command_bonuses_for(source,
            phase.impl_->command_ledger, ledgers, containers, categories, &staged), Impl::BonusAdjustment::loss); !applied)
            return core::Result<RejectReason>::failure(applied.error());
    }
    if (auto planned = replan(unit_id); !planned) return core::Result<RejectReason>::failure(planned.error());
    return core::Result<RejectReason>::success(RejectReason::none);
    }

core::Result<RejectReason> session_detail::Tick::CommandHandlers::apply_concentrate_fire(EntityId unit_id,
        const AbilityPayload& payload, const std::uint64_t& command_tick,
        std::map<EntityId, ArrivalState>& arrivals, std::vector<PlayerEconomy>& ledgers, UnitStage& staged,
        const detail::CombatWorld& view) const {
    auto& source = staged.at(unit_id);
    const auto* profile = phase.impl_->abilities.find(source.state.type_id);
    const auto ordinary = profile ? ability_slot(*profile, payload.ability) : std::nullopt;
    const auto* nested = phase.impl_->concentrate_profile(source);
    if (!source.abilities || !ordinary || nested == nullptr || source.abilities->special.service_cancelled) {
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    }
    const auto special_index = static_cast<std::size_t>(nested - profile->special.data());
    auto& special = source.abilities->special.slots[special_index];
    auto& state = source.abilities->slots[*ordinary];
    const auto& ability = profile->abilities[*ordinary];
    const auto refresh_target_speed = [&](const EntityId target_id) -> core::Result<void> {
        if (nested->target_speed_decrease.raw() == 0) return core::Result<void>::success();
        const auto recipient = staged.find(target_id);
        if (recipient == staged.end()) return core::Result<void>::success();
        // WHE-25, FD-01: use the existing command replan transaction, including
        // targets without an ability component to carry a pending replan flag.
        std::map<EntityId, TypeId> speed_containers;
        for (const auto& account : ledgers) for (const auto& held : account.completed)
            if (const auto station = staged.find(held.station); station != staged.end())
                speed_containers.emplace(held.station, station->second.state.type_id);
        std::vector<CombatBonuses> speed_categories(phase.impl_->bonus_categories.size());
        if (auto applied = phase.impl_->apply_bonuses(recipient->second, phase.impl_->command_bonuses_for(recipient->second,
            phase.impl_->command_ledger, ledgers, speed_containers, speed_categories, &staged), Impl::BonusAdjustment::loss); !applied)
            return core::Result<void>::failure(applied.error());
        return replan(target_id);
    };
    // AB-40, WAB-36: the autofire flag the attack-state pass reads, only where Supports_Autofire allows it.
    if (payload.action == AbilityAction::autofire_on || payload.action == AbilityAction::autofire_off) {
        if (!ability.supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        state.autofire = payload.action == AbilityAction::autofire_on;
        return core::Result<RejectReason>::success(RejectReason::none);
    }
    if (payload.action == AbilityAction::deactivate) {
        const auto previous_target = state.target;
        static_cast<void>(deactivate_ability(ability, state, command_tick));
        state.target = invalid_entity_id;
        state.target_hardpoint = no_hardpoint;
        source.abilities->concentrate_recruits.clear();
        special.targets.clear();
        special.context.reset();
        phase.impl_->register_concentrate(source);
        if (auto refreshed = refresh_target_speed(previous_target); !refreshed)
            return core::Result<RejectReason>::failure(refreshed.error());
        return core::Result<RejectReason>::success(RejectReason::none);
    }
    if (payload.action != AbilityAction::activate || state.active || special.cancelled || source.abilities->special.service_cancelled
        || nested->initially_enabled == false || nested->style != SpecialActivationStyle::user_input
        || !ability_ready(ability, state, phase.impl_->ability_gate(source, command_tick), command_tick)) {
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    }
    const auto target = staged.find(payload.target);
    if (target == staged.end()) return core::Result<RejectReason>::success(RejectReason::target_not_live);
    if (!players_hostile(phase.impl_->snapshot_players, source.state.owner, target->second.state.owner)) {
        return core::Result<RejectReason>::success(RejectReason::target_not_hostile);
    }
    const auto* target_profile = phase.impl_->combat.find(target->second.state.type_id);
    if (target_profile == nullptr || !special_type_matches(nested->filter, target->second.state.type_id,
        target_profile->category_bits)) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    if (payload.target_hardpoint != no_hardpoint && !phase.impl_->target_hardpoint_standing(target->second, payload.target_hardpoint)) {
        return core::Result<RejectReason>::success(RejectReason::hardpoint_invalid);
    }
    // WHE-24: one target-centred spatial query, then partitioned eligibility and team promotion.
    std::vector<std::uint32_t> candidates;
    std::uint64_t inspected = 0;
    view.index.box_positions(target->second.state.position,
        {ability.effective_radius, ability.effective_radius, ability.effective_radius}, candidates, &inspected);
    std::vector<EntityId> candidate_ids;
    candidate_ids.reserve(candidates.size() + concentrate_new_bodies.size());
    for (const auto position : candidates) candidate_ids.push_back(view.index.bodies()[position].entity_id);
    if (!concentrate_new_bodies.empty()) {
        const auto added_index = SpaceIndex::build(concentrate_new_bodies);
        if (!added_index) return core::Result<RejectReason>::failure(added_index.error());
        added_index.value().box_positions(target->second.state.position,
            {ability.effective_radius, ability.effective_radius, ability.effective_radius}, candidates, &inspected);
        for (const auto position : candidates) candidate_ids.push_back(added_index.value().bodies()[position].entity_id);
    }
    auto& work = phase.gather_->tick_work.value();
    ++work.concentrate_queries;
    work.concentrate_candidates += inspected;
    std::vector<EntityId> recruited(candidate_ids.size());
    std::vector<std::uint8_t> own_targeting(candidate_ids.size());
    const auto selected = phase.executor.execute_phase("concentrate-recruit", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, candidate_ids.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto id = candidate_ids[i];
            const auto found = staged.find(id);
            if (found == staged.end() || found->second.state.owner != source.state.owner
                || !found->second.combat || !within_range(target->second.state.position, found->second.state.position,
                    ability.effective_radius, RangeMetric::spatial)) continue;
            own_targeting[i] = 1;
            if (id == unit_id) continue;
            const auto team = phase.craft_prep_->craft_squadron.value().find(id);
            const auto promoted = team == phase.craft_prep_->craft_squadron.value().end() ? id : team->second;
            const auto parent = staged.find(promoted);
            if (parent == staged.end() || parent->second.state.owner != source.state.owner || promoted == unit_id
                || arrivals.contains(promoted)) continue;
            recruited[i] = promoted;
        }
    });
    if (!selected) return core::Result<RejectReason>::failure(selected.error());
    // WHE-24: an empty same-owner targeting query releases before starting a timer.
    if (std::none_of(own_targeting.begin(), own_targeting.end(), [](const auto match) { return match != 0; })) {
        return core::Result<RejectReason>::success(RejectReason::none);
    }
    std::erase(recruited, invalid_entity_id);
    std::sort(recruited.begin(), recruited.end());
    recruited.erase(std::unique(recruited.begin(), recruited.end()), recruited.end());
    source.abilities->concentrate_recruits = recruited;
    if (!recruited.empty()) {
        concentrate_batches.push_back({concentrate_command_key, std::move(recruited), AttackPayload{payload.target, payload.target_hardpoint}});
        ++work.concentrate_batches;
    }
    // Start the ordinary expiry only after the recruitment transaction succeeded.
    static_cast<void>(activate_ability(ability, state, phase.impl_->ability_gate(source, command_tick), command_tick));
    state.target = payload.target;
    state.target_hardpoint = payload.target_hardpoint;
    special.enabled = true;
    special.targets = {payload.target};
    special.next_service_frame = command_tick + nested->service_interval;
    phase.impl_->register_concentrate(source);
    if (auto refreshed = refresh_target_speed(state.target); !refreshed)
        return core::Result<RejectReason>::failure(refreshed.error());
    return core::Result<RejectReason>::success(RejectReason::none);
    }

core::Result<RejectReason> session_detail::Tick::CommandHandlers::apply_buzz_droids(EntityId unit_id,
        const AbilityPayload& payload, const std::uint64_t& command_tick, detail::MapStage<SquadronState>& minds,
        EntityId& next_id, const std::uint64_t& frame, UnitStage& staged, const SquadronTable& squadron_table,
        const MotionTable& table) const {
    auto& source = staged.at(unit_id);
    const auto* profile = phase.impl_->abilities.find(source.state.type_id);
    const auto index = profile != nullptr ? ability_slot(*profile, AbilityKind::buzz_droids) : std::nullopt;
    if (!index || !source.abilities || !profile->abilities[*index].buzz)
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    const auto& ability = profile->abilities[*index];
    const auto& buzz = *ability.buzz;
    auto& slot = source.abilities->slots[*index];
    if (payload.action == AbilityAction::deactivate) return core::Result<RejectReason>::success(RejectReason::none);
    if (payload.action == AbilityAction::autofire_on || payload.action == AbilityAction::autofire_off) {
        if (!ability.supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        slot.autofire = payload.action == AbilityAction::autofire_on;
        return core::Result<RejectReason>::success(RejectReason::none);
    }
    if (!payload.position || !ability_ready(ability, slot, phase.impl_->ability_gate(source, command_tick), command_tick)
        || next_id == invalid_entity_id || phase.impl_->durability.find(buzz.droid_type) == nullptr)
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    // WAB-70: the ordinary attack-range test against the point: planar, inclusive of the
    // authored maximum and exclusive of the minimum; a point has no extent.
    const auto point = *payload.position;
    // WAB-70/UFS-21: keep the admission refusal at or inside the minimum;
    // FS-23/FC-51 establish retries for admitted requests, not this admission.
    if (buzz.min_range.raw() > 0 && within_range(source.state.position, point, buzz.min_range, RangeMetric::planar))
        return core::Result<RejectReason>::success(RejectReason::invalid_position);
    if (const auto mind = minds.find(unit_id); mind != minds.end()) {
        // FS-23/FD-32: the admitted team event retains one special attack and gives
        // its holder the coordinated approach destination. FC-51 retries before scans.
        source.abilities->pending_buzz_position = point;
        source.state.order = order_for(MovePayload{point}, command_tick);
        auto& team = minds.at(unit_id);
        const auto target = team.target;
        const auto hardpoint = team.target_hardpoint;
        const auto formation = team.formation;
        apply_squadron_order(team, MovePayload{point}, source.state.position, squadron_table);
        team.target = target;
        team.target_hardpoint = hardpoint;
        // FS-23: the special point does not replace the ordinary attack target.
        team.formation->base_target = formation ? formation->base_target : invalid_entity_id;
        team.formation->attack_target = formation ? formation->attack_target : target;
        team.formation->attack_hardpoint = formation ? formation->attack_hardpoint : hardpoint;
        team.approach = false;
        return core::Result<RejectReason>::success(RejectReason::none);
    }
    if (!within_range(source.state.position, point, buzz.max_range, RangeMetric::planar))
        return core::Result<RejectReason>::success(RejectReason::invalid_position);
    // WAB-71: the holder stops, starts its full recharge and creates the droids at the
    // point, unturned, for its owner; the droids act from the arming frame on.
    auto droid = phase.impl_->create_buzz_droids(source, point, command_tick, next_id);
    const auto droid_id = droid.state.entity_id;
    if (squadron_table.find_spawner(droid.state.type_id, droid.state.owner) != nullptr)
        phase.staging_.value().born_spawners.push_back(droid_id);
    staged.emplace(droid_id, std::move(droid));
    phase.track_victory_change(nullptr, &staged.at(droid_id).state, false);
    auto& live = staged.at(unit_id); // insertion can move the vector
    if (minds.find(unit_id) != minds.end()) {
        apply_squadron_order(minds.at(unit_id), StopPayload{}, live.state.position, squadron_table);
    } else if (live.motion) {
        if (auto stopped = phase.impl_->plan_order(live, StopPayload{}, frame); !stopped)
            return core::Result<RejectReason>::failure(stopped.error());
        const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
        if (footprint) {
            if (const auto layer = dynamic_layer_index(footprint->layer)) {
                if (auto submitted = submit(unit_id, live, *layer, std::nullopt); !submitted)
                    return core::Result<RejectReason>::failure(submitted.error());
            }
        }
    }
    return core::Result<RejectReason>::success(RejectReason::none);
    }

core::Result<RejectReason> session_detail::Tick::CommandHandlers::apply_ion_cannon_shot(EntityId unit_id,
        const AbilityPayload& payload, const std::uint64_t& command_tick, detail::MapStage<SquadronState>& minds,
        UnitStage& staged, const SquadronTable& squadron_table) const {
    auto& container = staged.at(unit_id);
    auto* team = phase.impl_->ion_slot(container);
    const auto squadron = std::find_if(phase.impl_->squadrons.begin(), phase.impl_->squadrons.end(),
        [&](const Squadron& entry) { return entry.container == unit_id; });
    const auto mind = minds.find(unit_id);
    if (team == nullptr || squadron == phase.impl_->squadrons.end() || mind == minds.end()) {
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    }
    std::vector<AbilitySlot*> craft;
    for (const auto member : squadron->members) {
        const auto found = staged.find(member);
        if (auto* slot = found != staged.end() ? phase.impl_->ion_slot(found->second) : nullptr) craft.push_back(slot);
    }
    const auto& profile = *phase.impl_->abilities.find(container.state.type_id);
    const auto& ability = profile.abilities[*ability_slot(profile, AbilityKind::ion_cannon_shot)];
    switch (payload.action) {
    case AbilityAction::activate: {
        // AB-62: ready, off, with a craft to fire it and none still due; a valid target.
        const bool due = std::any_of(craft.begin(), craft.end(), [](const AbilitySlot* slot) { return slot->active; });
        if (team->active || command_tick < team->ready_tick || craft.empty() || due
            || (container.nebula && container.nebula->present)) {
            return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        }
        const auto target = staged.find(payload.target);
        if (target == staged.end()) return core::Result<RejectReason>::success(RejectReason::target_not_live);
        if (!phase.impl_->ion_target_valid(container.state.owner, &target->second)) {
            return core::Result<RejectReason>::success(RejectReason::target_not_hostile);
        }
        const auto* hit = phase.impl_->health_profile(target->second);
        const auto hardpoint = hit != nullptr && payload.target_hardpoint != no_hardpoint
                && damage_target_valid(*hit, payload.target_hardpoint)
            ? payload.target_hardpoint
            : no_hardpoint;
        team->active = true;
        team->started_tick = command_tick;
        team->target = payload.target;
        team->target_hardpoint = hardpoint;
        for (auto* slot : craft) slot->active = true;
        // AB-63: the squadron attacks the target.
        const CommandPayload attack = AttackPayload{payload.target, hardpoint};
        container.state.order = order_for(attack, command_tick);
        apply_squadron_order(minds.at(mind->first), attack, container.state.position, squadron_table);
        break;
    }
    case AbilityAction::deactivate:
        if (team->active) phase.impl_->finish_ion_shot(*team, craft, container.state.type_id, command_tick);
        break;
    case AbilityAction::autofire_on:
    case AbilityAction::autofire_off:
        if (!ability.supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        team->autofire = payload.action == AbilityAction::autofire_on;
        break;
    }
    return core::Result<RejectReason>::success(RejectReason::none);
    }

core::Result<RejectReason> session_detail::Tick::CommandHandlers::switch_ordinary_ability(
        const AbilityPayload& payload, const std::vector<LiveUnit*>& holders, auto& slot_of, auto& profile_of,
        const std::uint64_t& command_tick, const std::uint64_t& frame, std::vector<PlayerEconomy>& ledgers,
        detail::MapStage<CraftState>& crafts, UnitStage& staged, std::vector<EntityId>& changed_speed,
        bool& hunt_stopped) const {
    switch (payload.action) {
    case AbilityAction::activate:
        for (auto* live : holders) {
            const auto& slot = slot_of(*live);
            if (slot.active || !ability_ready(profile_of(*live), slot, phase.impl_->ability_gate(*live, command_tick), command_tick)) {
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            }
        }
        for (auto* live : holders) {
            const auto switched = activate_ability(profile_of(*live), slot_of(*live), phase.impl_->ability_gate(*live, command_tick), command_tick);
            if (switched.changed && (payload.ability == AbilityKind::interdict || payload.ability == AbilityKind::missile_shield)) {
                // WIN-02, WMS-02: deployment halts the current move even without a movement lock.
                live->formation.reset();
                live->state.order = order_for(StopPayload{}, command_tick);
                if (auto stopped = phase.impl_->plan_order(*live, StopPayload{}, frame); !stopped)
                    return core::Result<RejectReason>::failure(stopped.error());
            }
            if (switched.changed && payload.ability == AbilityKind::sensor_jamming) phase.register_jammer(live->state.entity_id, true);
            if (switched.changed && payload.ability == AbilityKind::defend) phase.impl_->defend_switched_on(*live, command_tick);
            if (switched.changed && payload.ability == AbilityKind::self_destruct) Impl::self_destruct_switched(*live);
            if (switched.changed && payload.ability == AbilityKind::stealth) {
                phase.impl_->stealth_switched_on(*live, command_tick, phase.impl_->stealth_duration_factor(live->state.owner, ledgers));
                if (crafts.find(live->state.entity_id) != crafts.end()) {
                    crafts.at(live->state.entity_id).chase = invalid_entity_id;
                    crafts.at(live->state.entity_id).chase_until = 0;
                }
            }
            if (switched.changed && (payload.ability == AbilityKind::hunt || payload.ability == AbilityKind::lure)) {
                // WAB-04, WLU-02: clear attack targets; HUNT keeps the movement under way.
                if (live->combat) {
                    live->combat->attack_target = invalid_entity_id;
                    live->combat->attack_hardpoint = no_hardpoint;
                    live->combat->direct = false;
                }
                if (live->state.order.target != invalid_entity_id) live->state.order = {};
                if (crafts.find(live->state.entity_id) != crafts.end()) {
                    crafts.at(live->state.entity_id).chase = invalid_entity_id;
                    crafts.at(live->state.entity_id).chase_until = 0;
                }
            }
            if (switched.speed) changed_speed.push_back(live->state.entity_id);
        }
        break;
    case AbilityAction::deactivate:
        for (auto* live : holders) {
            auto& slot = slot_of(*live);
            const auto switched = deactivate_ability(profile_of(*live), slot, command_tick);
            if (switched.changed && payload.ability == AbilityKind::sensor_jamming) phase.register_jammer(live->state.entity_id, false);
            if (switched.changed && payload.ability == AbilityKind::stealth) Impl::clear_stealth(*live); // WST-05
            if (switched.speed) changed_speed.push_back(live->state.entity_id);
            if (switched.changed && payload.ability == AbilityKind::self_destruct) Impl::self_destruct_switched(*live);
            hunt_stopped = hunt_stopped || (switched.changed
                && (payload.ability == AbilityKind::hunt || payload.ability == AbilityKind::lure));
            if (switched.changed && payload.ability == AbilityKind::hunt) {
                if (live->combat) {
                    live->combat->attack_target = invalid_entity_id;
                    live->combat->attack_hardpoint = no_hardpoint;
                    live->combat->direct = false;
                }
                if (crafts.find(live->state.entity_id) != crafts.end()) {
                    crafts.at(live->state.entity_id).chase = invalid_entity_id;
                    crafts.at(live->state.entity_id).chase_until = 0;
                }
            }
            if (payload.ability == AbilityKind::barrage) {
                if (const auto proxy = staged.find(slot.target); proxy != staged.end()
                    && proxy->second.state.barrage_source == live->state.entity_id) {
                    phase.track_victory_change(&proxy->second.state, nullptr, true);
                    staged.erase(proxy);
                }
                if (live->combat && live->combat->attack_target == slot.target) {
                    live->combat->attack_target = invalid_entity_id;
                    live->combat->direct = false;
                }
                if (live->state.order.target == slot.target) live->state.order = {};
                slot.target = invalid_entity_id;
            }
        }
        break;
    case AbilityAction::autofire_on:
    case AbilityAction::autofire_off:
        for (auto* live : holders) {
            if (!profile_of(*live).supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        }
        for (auto* live : holders) slot_of(*live).autofire = payload.action == AbilityAction::autofire_on;
        break;
    }
    return core::Result<RejectReason>::success(RejectReason::none);
}

core::Result<RejectReason> session_detail::Tick::CommandHandlers::apply_ordinary_ability(EntityId unit_id,
        const AbilityPayload& payload, const std::uint64_t& command_tick, detail::MapStage<SquadronState>& minds,
        detail::MapStage<CraftState>& crafts, std::vector<PlayerEconomy>& ledgers, const std::uint64_t& frame,
        UnitStage& staged, const SquadronTable& squadron_table, const MotionTable& table) const {
    const auto holders = ordinary_ability_holders(unit_id, payload.ability);
    const auto holds = [&](LiveUnit& live) -> std::optional<std::size_t> {
        if (!live.abilities) return std::nullopt;
        return ability_slot(*phase.impl_->abilities.find(live.state.type_id), payload.ability);
    };
    if (holders.empty()) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    const auto profile_of = [&](const LiveUnit& live) -> const AbilityProfile& {
        const auto& profile = *phase.impl_->abilities.find(live.state.type_id);
        return profile.abilities[*ability_slot(profile, payload.ability)];
    };
    const auto slot_of = [&](LiveUnit& live) -> AbilitySlot& { return live.abilities->slots[*holds(live)]; };
    const auto original_order = staged.at(unit_id).state.order;
    std::vector<EntityId> changed_speed;
    bool hunt_stopped = false; // also LURE: WAB-05 and WLU-02 stop the behaviour's move
    auto switched = switch_ordinary_ability(payload, holders, slot_of, profile_of, command_tick, frame, ledgers, crafts, staged, changed_speed, hunt_stopped);
    if (!switched || switched.value() != RejectReason::none) return switched;
    const bool behaviour_on = (payload.ability == AbilityKind::hunt || payload.ability == AbilityKind::lure
        || payload.ability == AbilityKind::stealth)
        && payload.action == AbilityAction::activate;
    // WLU-02 stops the move; HUNT and STEALTH retain their own destination rules.
    if (behaviour_on && payload.ability == AbilityKind::lure) hunt_stopped = true;
    if (behaviour_on) {
        const auto& membership = phase.craft_prep_->craft_squadron.value();
        const auto member = membership.find(unit_id);
        const auto container_id = member != membership.end() ? member->second : unit_id;
        // WST-03 applies whole-team release to a team request. A member-addressed cloak
        // keeps the other members' order; it does not claim the team's destination.
        if (payload.ability != AbilityKind::stealth || container_id == unit_id) {
            auto& container = staged.at(container_id);
            bool preserve_escort = false;
            if (const auto mind = minds.find(container_id); mind != minds.end()) {
                // WST-03: release the team's attack as well as the members' targets. Otherwise
                // the old player attack is shared back as a direct order on the next turn.
                auto& team = minds.at(container_id);
                const auto escort = staged.find(team.escorted);
                preserve_escort = payload.ability == AbilityKind::stealth && team.mode == SquadronMode::escort
                    && escort != staged.end() && !players_hostile(phase.impl_->snapshot_players,
                        container.state.owner, escort->second.state.owner);
                if (payload.ability == AbilityKind::stealth && !preserve_escort
                    && (team.target != invalid_entity_id || team.escorted != invalid_entity_id
                        || (team.formation && team.formation->attack_target != invalid_entity_id))) {
                    auto destination = container.state.position;
                    if (team.attack_path && !team.attack_path->stack.empty()
                        && !team.attack_path->stack.back().nodes.empty())
                        destination = team.attack_path->stack.back().nodes.back();
                    apply_squadron_order(team, MovePayload{destination}, container.state.position, squadron_table);
                    container.state.order = order_for(MovePayload{destination}, command_tick);
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
                }
                team.target = invalid_entity_id;
                team.target_hardpoint = attack_hull;
                team.approach = false;
            }
            if (container.combat) {
                container.combat->attack_target = invalid_entity_id;
                container.combat->direct = false;
            }
            if (preserve_escort) container.state.order = original_order;
            else if (container.state.order.target != invalid_entity_id) container.state.order = {};
        }
    }
    if (hunt_stopped) {
        // WAB-05: disabling the behaviour also stops its current movement.
        const auto& membership = phase.craft_prep_->craft_squadron.value();
        const auto member = membership.find(unit_id);
        const auto container_id = member != membership.end() ? member->second : unit_id;
        auto& live = staged.at(container_id);
        live.state.order = order_for(StopPayload{}, command_tick);
        if (minds.find(container_id) != minds.end()) {
            apply_squadron_order(minds.at(container_id), StopPayload{}, live.state.position, squadron_table);
        } else {
            if (auto stopped = phase.impl_->plan_order(live, StopPayload{}, frame); !stopped) return core::Result<RejectReason>::failure(stopped.error());
            const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
            if (footprint && live.motion) {
                if (const auto layer = dynamic_layer_index(footprint->layer)) {
                    if (auto submitted = submit(container_id, live, *layer, std::nullopt); !submitted) return core::Result<RejectReason>::failure(submitted.error());
                }
            }
        }
    }
    for (const auto id : changed_speed) {
        if (auto planned = replan(id); !planned) return core::Result<RejectReason>::failure(planned.error());
    }
    // WHE-51: later commands and environmental hits read the newly switched modes.
    for (auto* live : holders) phase.impl_->refresh_damage_modes(*live);
    return core::Result<RejectReason>::success(RejectReason::none);
}

core::Result<RejectReason> session_detail::Tick::CommandHandlers::apply_ability(const EntityId unit_id, const AbilityPayload& payload) const {
    const auto command_tick = phase.tick;
    auto& reinforcement_work = phase.gather_.value().reinforcement_work.value();
    auto& minds = phase.craft_prep_.value().minds.value();
    auto& crafts = phase.staging_.value().crafts.value();
    auto& arrivals = phase.staging_.value().arrivals.value();
    auto& ledgers = phase.staging_.value().ledgers.value();
    auto& pad_requests = phase.staging_.value().pad_requests.value();
    auto& shares = phase.staging_.value().shares.value();
    auto& arrived_squadrons = phase.staging_.value().arrived_squadrons.value();
    auto& earners = phase.staging_.value().earners.value();
    auto& next_id = phase.staging_.value().next_id.value();
    const auto& world = phase.targets_.value().world.value();
    const auto& frame = phase.tracked_.value().frame.value();
    auto& staged = phase.tracked_.value().staged.value();
    const auto& squadron_table = phase.impl_->motion.squadrons;
    auto& pads = phase.impl_->pad_stage.values;
    auto& construction = phase.impl_->construction_stage.values;
    const auto& view = world.value();
    const auto& table = phase.impl_->motion;
    if (payload.ability == AbilityKind::blast) {
        return apply_blast(unit_id, payload, command_tick, staged);
    }
    if (payload.ability == AbilityKind::corrupt_systems) {
        return apply_corrupt_systems(unit_id, payload, command_tick, arrivals, frame, staged, table);
    }
    if (payload.ability == AbilityKind::replenish_wingmen) {
        return apply_replenish_wingmen(unit_id, payload, command_tick, reinforcement_work, minds, crafts, arrivals, ledgers, pad_requests, shares, arrived_squadrons, earners, next_id, staged, squadron_table, pads, construction);
    }
    if (payload.ability == AbilityKind::harmonic_bomb || payload.ability == AbilityKind::weaken_enemy
        || payload.ability == AbilityKind::cluster_bomb || payload.ability == AbilityKind::fow_reveal_ping) {
        return apply_spawned_ability(unit_id, payload, command_tick);
    }
    if (payload.ability == AbilityKind::energy_weapon || payload.ability == AbilityKind::tractor_beam
        || payload.ability == AbilityKind::leech_shields) {
        return apply_beam_ability(unit_id, payload, command_tick, ledgers, staged);
    }
    if (payload.ability == AbilityKind::concentrate_fire) {
        return apply_concentrate_fire(unit_id, payload, command_tick, arrivals, ledgers, staged, view);
    }
    if (payload.ability == AbilityKind::barrage && payload.action == AbilityAction::activate) {
        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
    }
    // WAB-70/71: BUZZ_DROIDS acts on its holder (a squadron's team container, AB-60) at a point.
    if (payload.ability == AbilityKind::buzz_droids) {
        return apply_buzz_droids(unit_id, payload, command_tick, minds, next_id, frame, staged, squadron_table, table);
    }
    // AB-61 to AB-65 (#561): ION_CANNON_SHOT acts on the squadron's team container itself.
    if (payload.ability == AbilityKind::ion_cannon_shot) {
        return apply_ion_cannon_shot(unit_id, payload, command_tick, minds, staged, squadron_table);
    }
    return apply_ordinary_ability(unit_id, payload, command_tick, minds, crafts, ledgers, frame, staged, squadron_table, table);
}

} // namespace eawr::sim::tactical
