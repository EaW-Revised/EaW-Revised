#include "eawr/sim/tactical/session.hpp"

#include "eawr/sim/tactical/tsw.hpp"

#include "session_cmd.hpp"
#include "session_services.hpp"
#include "session_tick.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

// The tactical super weapon service (TSW-02..TSW-11, docs/behaviour/space-tactical-superweapon.md).
//
// Why serial: this is at most one or two objects per battle, and its effects are ordered (a request changes the same
// record the service then advances, the sequence holds and releases the battle's victory tests, and the destruction
// commits the planet fact), so it runs in the tick's serial commit region next to the special-weapon service instead of
// in a partitioned job (ADR-009: no per-entity loop is added to the tick; the loop here is over the TSW records, not
// the units).
namespace eawr::sim::tactical {

namespace {

[[nodiscard]] FactionId faction_of(const std::vector<Player>& players, const PlayerId owner) {
    const auto found = std::find_if(players.begin(), players.end(), [&](const Player& player) { return player.player_id == owner; });
    return found == players.end() ? FactionId{} : found->faction_id;
}

// TSW-03: the defender's object starts in range, so its first service puts it straight into ready.
[[nodiscard]] bool is_defender(const TswSetup& setup, const PlayerId owner) {
    return setup.conflict && setup.conflict->defender != 0 && setup.conflict->defender == owner;
}

} // namespace

std::vector<TswView> TacticalSession::Impl::tsw_views(const std::span<const TacticalInstance> instances) const {
    // TSW-13: a copy of the committed records for the switch presenter, one row per object that still has its
    // behaviour, ascending id (the map's order). No state is read that the hash does not carry.
    std::vector<TswView> rows;
    if (tsw_states.empty() || !setup.tsw) return rows;
    const bool gate = ::eawr::sim::tactical::tsw_gate_open(setup.tsw->conflict) && !tsw_world.planet_destroyed;
    for (const auto& [id, state] : tsw_states) {
        const auto instance = std::find_if(instances.begin(), instances.end(),
            [id = id](const TacticalInstance& candidate) { return candidate.entity_id == id; });
        if (instance == instances.end()) continue;
        const auto* profile = combat.find(instance->type_id);
        if (profile == nullptr || !profile->tactical_super_weapon) continue;
        TswView row;
        row.unit = id;
        row.owner = instance->owner;
        row.phase = state.phase;
        row.serviced = state.serviced;
        row.has_target = state.target != 0;
        row.gate_open = gate;
        if (row.has_target && state.target <= setup.tsw->targets.size()
            && (state.phase == TswPhase::facing || state.phase == TswPhase::approaching || state.phase == TswPhase::ready)) {
            const math::Vec3 position{instance->fixed_transform.rows[0][3], instance->fixed_transform.rows[1][3],
                instance->fixed_transform.rows[2][3]};
            // Ready is in range by definition (the object still coasts to a stop): the readout shows zero.
            if (state.phase == TswPhase::ready) {
                row.eta_frames = 0;
            } else if (const auto eta = tsw_eta_frames(*profile->tactical_super_weapon, position,
                           setup.tsw->targets[state.target - 1].position)) {
                row.eta_frames = eta.value();
            }
        }
        rows.push_back(row);
    }
    return rows;
}

core::Result<void> session_detail::Tick::tsw_command(
    const PlayerCommand& command, const TacticalSuperweaponPayload& payload, const PlayerId issuer) {
    auto& staged = tracked_.value().staged.value();
    auto& events = impacts_.value().events.value();
    auto& diagnostics = commands_->diagnostics.value();
    const EntityId unit_id = command.units.empty() ? invalid_entity_id : command.units.front();
    auto reason = RejectReason::none;
    const auto unit = staged.find(unit_id);
    const auto record = tsw_.find(unit_id);
    if (unit == staged.end()) {
        reason = RejectReason::unit_not_live;
    } else if (unit->second.state.owner != issuer) {
        reason = RejectReason::unit_not_owned;
    } else if (session_detail::controller_admission(impl_->setup, command, unit->second.state) != RejectReason::none) {
        reason = RejectReason::unit_not_controlled;
    } else if (record == tsw_.end() || !impl_->setup.tsw) {
        reason = RejectReason::ability_unavailable; // no super weapon behaviour, or a battle without its setup
    } else {
        // TSW-07: the gate and the phase decide. The gate also closes once this battle destroyed the planet (TSW-10).
        // A defender's object that has not had its first service yet is already ready by TSW-03 (that service would put
        // it there this tick), so its approach request is refused.
        const bool unserviced_defender = !record->second.serviced && is_defender(*impl_->setup.tsw, issuer);
        const bool gate = ::eawr::sim::tactical::tsw_gate_open(impl_->setup.tsw->conflict) && !tsw_world_.planet_destroyed;
        const bool accepted = !(payload.stage == TswStage::start_approach && unserviced_defender)
            && tsw_request_accepted(payload.stage, record->second.phase, gate);
        const auto* profile = impl_->combat.find(unit->second.state.type_id);
        if (!accepted) {
            reason = RejectReason::ability_unavailable;
        } else if (payload.stage == TswStage::start_power_up) {
            // A ready object always has its target; the power-up transition needs the type's numbers.
            if (record->second.target == 0 || profile == nullptr || !profile->tactical_super_weapon)
                reason = RejectReason::ability_unavailable;
        } else {
            // The click picks the target as the first service does (TSW-03); with none there is nothing to approach.
            const auto selected = tsw_select_target(*impl_->setup.tsw, faction_of(impl_->setup.players, issuer),
                unit->second.state.position, tsw_world_.destroyed_targets);
            if (!selected) return core::Result<void>::failure(selected.error());
            if (selected.value() == 0) reason = RejectReason::ability_unavailable;
            else {
                record->second.target = selected.value();
                record->second.serviced = true; // a request before the first service is the first service
            }
        }
        if (reason == RejectReason::none && payload.stage == TswStage::start_power_up) {
            // TSW-08: S4. The countdown starts, the object stops where it is, and the battle's victory tests are held
            // until the sequence ends (TSW-09). The service of this same tick counts the first countdown frame.
            record->second.phase = TswPhase::powering_up;
            record->second.countdown = profile->tactical_super_weapon->power_up_frames;
            record->second.speed = math::Fixed{};
            ++tsw_world_.victory_hold;
            events.push_back(Event{tick, EventKind::order_accepted, issuer, command.key.sequence, unit_id,
                OrderKind::tactical_superweapon});
            events.push_back(Event{tick, EventKind::tsw_power_up_started, issuer, record->second.target, unit_id});
            return core::Result<void>::success();
        }
    }
    if (reason != RejectReason::none) {
        events.push_back(Event{tick, EventKind::order_rejected, issuer, command.key.sequence, unit_id,
            OrderKind::tactical_superweapon, reason});
        diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
            command_context(command.key) + ": super weapon switch rejected (" + std::string(to_string(reason)) + ")",
            {}, core::Severity::warning));
        return core::Result<void>::success();
    }
    events.push_back(Event{tick, EventKind::order_accepted, issuer, command.key.sequence, unit_id,
        OrderKind::tactical_superweapon});
    record->second.phase = TswPhase::facing;
    events.push_back(Event{tick, EventKind::tsw_approach_started, issuer, 0, unit_id});
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::tsw_service() {
    if (tsw_.empty() || !impl_->setup.tsw) return core::Result<void>::success();
    auto& staged = tracked_.value().staged.value();
    auto& events = impacts_.value().events.value();
    const auto& setup = *impl_->setup.tsw;
    std::vector<EntityId> gone;
    std::uint32_t released = 0;
    std::vector<std::uint32_t> announce;
    for (auto& [id, state] : tsw_) {
        const auto unit = staged.find(id);
        const auto* profile = unit == staged.end() ? nullptr : impl_->combat.find(unit->second.state.type_id);
        if (profile == nullptr || !profile->tactical_super_weapon) {
            // A destroyed object leaves the service with its record. A sequence it held ends with it: the victory tests
            // it deferred run (unverified: the debug build's behaviour for an object lost mid-sequence was not walked).
            gone.push_back(id);
            if (tsw_holds_victory(state.phase)) ++released;
            continue;
        }
        auto& live = unit->second;
        const auto& numbers = *profile->tactical_super_weapon;
        const auto owner = live.state.owner;
        if (!state.serviced) {
            // TSW-03: the first service. The placement (start or attack distance) was done when the battle was built.
            state.serviced = true;
            const auto selected = tsw_select_target(setup, faction_of(impl_->setup.players, owner), live.state.position,
                tsw_world_.destroyed_targets);
            if (!selected) return core::Result<void>::failure(selected.error());
            state.target = selected.value();
            if (state.target != 0) {
                if (is_defender(setup, owner)) {
                    state.phase = TswPhase::ready;
                } else {
                    state.phase = TswPhase::facing; // the attacker flies in by itself; no click starts it
                    events.push_back(Event{tick, EventKind::tsw_approach_started, owner, 0, id});
                }
            }
        }
        if (state.target == 0) continue; // no target: it only faces the battlefield (or the sequence is over)
        const auto& target = setup.targets[state.target - 1];
        if (state.phase >= TswPhase::powering_up) {
            // TSW-08..TSW-11: the fire sequence.
            const auto action = tsw_step_sequence(id, owner, state, numbers);
            if (!action) return core::Result<void>::failure(action.error());
            if (action.value() == TswAction::attack_started) {
                // TSW-05: the facing is applied again at the frame the attack animation starts.
                const auto facing = tsw_facing(live.state.position, target.position);
                if (!facing) return core::Result<void>::failure(facing.error());
                if (facing.value()) {
                    live.state.rotation = *facing.value();
                    live.placement_cache.reset();
                    live.banked_placement_cache.reset();
                }
            }
        } else if (state.phase == TswPhase::facing) {
            // TSW-05: the facing state is one frame: an instant full 3D facing from the object to its target (heading
            // and elevation), then straight to the approach. No turn rate, no stop condition (debug build).
            const auto facing = tsw_facing(live.state.position, target.position);
            if (!facing) return core::Result<void>::failure(facing.error());
            if (facing.value()) {
                live.state.rotation = *facing.value();
                live.placement_cache.reset();
                live.banked_placement_cache.reset();
            }
            state.phase = TswPhase::approaching;
        } else if (state.phase == TswPhase::approaching) {
            const auto step = tsw_approach_step(numbers, live.state.position, state.speed, target.position);
            if (!step) return core::Result<void>::failure(step.error());
            if (step.value().in_range) {
                state.phase = TswPhase::ready;
                events.push_back(Event{tick, EventKind::tsw_in_range, owner, 0, id});
            } else {
                live.state.position = step.value().position;
                live.placement_cache.reset();
                live.banked_placement_cache.reset();
                state.speed = step.value().speed;
                // TSW-06 (debug build walk): the threshold check only runs while the speed equals Max_Speed and the
                // range is not reached, so the thrust ramp announces nothing; a threshold passed by then is announced on
                // the first frame at cruise speed.
                if (state.speed == numbers.max_speed && numbers.max_speed != math::Fixed{}) {
                    const auto eta = tsw_eta_frames(numbers, live.state.position, target.position);
                    if (!eta) return core::Result<void>::failure(eta.error());
                    announce.clear();
                    state.speech_mask = tsw_speech_crossings(eta.value(), state.speech_mask, announce);
                    for (const auto seconds : announce)
                        events.push_back(Event{tick, EventKind::tsw_speech_threshold, owner, seconds, id});
                }
            }
        } else if (state.phase == TswPhase::ready) {
            // TSW-05: it keeps slowing by Max_Thrust per frame and still moves while the speed is above zero; it does not
            // turn or re-face (the heading is the one the facing state set).
            const auto step = tsw_ready_step(numbers, live.state.position, state.speed, target.position);
            if (!step) return core::Result<void>::failure(step.error());
            if (step.value().position != live.state.position) {
                live.state.position = step.value().position;
                live.placement_cache.reset();
                live.banked_placement_cache.reset();
            }
            state.speed = step.value().speed;
        }
    }
    for (const auto id : gone) tsw_.erase(id);
    for (std::uint32_t index = 0; index < released; ++index)
        if (auto result = tsw_release_hold(); !result) return result;
    return core::Result<void>::success();
}

core::Result<TswAction> session_detail::Tick::tsw_step_sequence(
    const EntityId id, const PlayerId owner, TswState& state, const TswProfile& numbers) {
    auto& events = impacts_.value().events.value();
    const auto step = tsw_sequence_step(numbers, state.phase, state.countdown);
    state.phase = step.phase;
    state.countdown = step.countdown;
    const auto target = state.target;
    switch (step.action) {
    case TswAction::none: break;
    case TswAction::attack_started:
        events.push_back(Event{tick, EventKind::tsw_attack_started, owner, target, id});
        break;
    case TswAction::destroy_target: {
        // TSW-10: the target prop is destroyed, then the planet. The removal is a fact (the destroyed list), the rest is
        // presentation; the planet commit is handed to the galactic side as one event.
        const auto position = std::lower_bound(tsw_world_.destroyed_targets.begin(), tsw_world_.destroyed_targets.end(), target);
        if (position == tsw_world_.destroyed_targets.end() || *position != target) tsw_world_.destroyed_targets.insert(position, target);
        // The planet fact flips once: a second object whose sequence was already running (two super weapons, TSW-10)
        // still ends its own sequence, but the hand-off to the galactic side is published only by the first (unverified
        // in the debug build: the walk covered one object).
        const bool first_destruction = !tsw_world_.planet_destroyed;
        tsw_world_.planet_destroyed = true;
        events.push_back(Event{tick, EventKind::tsw_target_destroyed, owner, target, id});
        if (first_destruction) events.push_back(Event{tick, EventKind::planet_obliterated, owner, target, id});
        break;
    }
    case TswAction::create_debris:
        events.push_back(Event{tick, EventKind::tsw_debris_created, owner, target, id});
        break;
    case TswAction::finish: {
        // TSW-11: back to idle, the target cleared, the hold released.
        state.target = 0;
        events.push_back(Event{tick, EventKind::tsw_sequence_complete, owner, target, id});
        if (auto released = tsw_release_hold(); !released) return core::Result<TswAction>::failure(released.error());
        break;
    }
    }
    return core::Result<TswAction>::success(step.action);
}

core::Result<void> session_detail::Tick::tsw_release_hold() {
    // TSW-09: one sequence ends. At zero the deferred victory tests run again, in the order they were recorded, and the
    // list is dropped either way (the retry does not keep it).
    if (tsw_world_.victory_hold > 0) --tsw_world_.victory_hold;
    if (tsw_world_.victory_hold > 0) return core::Result<void>::success();
    auto deferred = std::move(tsw_world_.deferred);
    tsw_world_.deferred.clear();
    for (const auto& test : deferred) {
        if (test.kind == TswDeferredKind::unit) victory_unit_test(test.owner, test.unit);
        else victory_leader_test(test.owner, test.unit, test.kind);
    }
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
