#include "session_impl.hpp"
#include "eawr/sim/galactic/fleets.hpp"
#include "encoding.hpp"
#include "native_fleet_admission.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::galactic {
using session_detail::FleetData;

core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::admit_native_fleet(
    PlayerId player, const CommandPayload& payload, bool pending_local_conflict) {
    using Result = core::Result<NativeFleetReceipt>;
    auto& impl = *impl_;
    const bool fleet_command = std::holds_alternative<CreateFleet>(payload)
        || std::holds_alternative<AddFleetMembers>(payload) || std::holds_alternative<RemoveFleetMembers>(payload)
        || std::holds_alternative<MergeFleets>(payload) || std::holds_alternative<SetFleetOrbitSlot>(payload)
        || std::holds_alternative<TransferFleetMembers>(payload);
    const bool movement = std::holds_alternative<MoveFleet>(payload);
    if (impl.failed || encode_payload(payload).size() > galactic_replay_max_payload - 38 || (!fleet_command && !movement)
        || decode_payload(layer_of(payload), opcode_of(payload), encode_payload(payload)) != std::optional(payload))
        return Result::failure(detail::diagnostic(diagnostic_codes::invalid_command, "invalid native fleet command boundary"));
    const auto issuer = std::find_if(impl.seats.begin(), impl.seats.end(), [&](const auto& seat) {
        return seat.player == player && seat.active;
    });
    if (issuer == impl.seats.end()) return Result::success({false, 0, GalacticRejectReason::inactive_seat});
    if (auto* shadow = native_shadow()) return stage_native_fleet(*shadow, player, payload, pending_local_conflict);
    GameCommand command; command.key = {impl.completed_tick, issuer->seat_id, 0}; command.payload = payload;
    std::vector<GalacticEvent> events(impl.snapshot->events().begin(), impl.snapshot->events().end());
    const auto first = events.size();
    if (movement) impl.execute_movement(command, impl.completed_tick, events, pending_local_conflict);
    else impl.execute_fleet(command, impl.completed_tick, events, pending_local_conflict);
    NativeFleetReceipt receipt;
    for (auto i = first; i < events.size(); ++i) {
        if (events[i].kind == GalacticEventKind::command_rejected) receipt.reason = events[i].reason;
        if (events[i].kind == GalacticEventKind::command_executed) {
            receipt.admitted = true; receipt.object = events[i].subject;
        }
    }
    impl.snapshot = impl.publish_native_snapshot(std::move(events));
    if (auto journaled = impl.journal_native_receipt(*this, NativeEffectKind::fleet, player, 0, 0,
        pending_local_conflict, &payload, receipt); !journaled) return Result::failure(journaled.error());
    // A launch owns the temporary detached row and publishes its final surface
    // removal itself. Do not deliver against that nested admission's view.
    if (!impl.native_applying) if (auto delivered = impl.deliver_actor_changes(); !delivered)
        return Result::failure(delivered.error());
    return Result::success(receipt);
}

core::Result<bool> fleet_move_without_ability(const Fleet& fleet, const std::span<const FleetTypeTraits> types,
    const MovementSetup* movement) {
    bool qualifying = false, nonempty = false;
    const auto trait = [&](const FleetMember& member) {
        const auto found = std::lower_bound(types.begin(), types.end(), member.type,
            [](const auto& row, const auto type) { return row.type < type; });
        return found == types.end() || found->type != member.type ? nullptr : &*found;
    };
    const auto inspect = [&](const auto& self, const FleetMember& member, unsigned depth) -> bool {
        const auto* found = trait(member);
        if (!found || depth > 32) return false;
        if (found->requires_live_children) {
            if (!movement) return false;
            const auto contents = std::lower_bound(movement->contents.begin(), movement->contents.end(), member.id,
                [](const auto& row, const auto id) { return row.container < id; });
            if (contents == movement->contents.end() || contents->container != member.id) return false;
            // WGM-83/101: inspect surviving craft, not the squadron/container template.
            for (const auto& child : contents->children) if (!self(self, child, depth + 1)) return false;
            return true;
        }
        nonempty = true;
        qualifying = qualifying || (!found->transport && found->can_hyperspace_without_ability);
        if (!found->transport || !movement) return true;
        // WGM-83 (debug build): the fleet breakdown also lists a transport's carried units, one level,
        // so troops on a transport supply the qualifying nontransport.
        const auto contents = std::lower_bound(movement->contents.begin(), movement->contents.end(), member.id,
            [](const auto& row, const auto id) { return row.container < id; });
        if (contents == movement->contents.end() || contents->container != member.id) return true;
        for (const auto& carried : contents->children) {
            const auto* carried_trait = trait(carried);
            if (!carried_trait) return false;
            qualifying = qualifying || (!carried_trait->transport && carried_trait->can_hyperspace_without_ability);
        }
        return true;
    };
    for (const auto& member : fleet.members) {
        if (!inspect(inspect, member, 0))
            return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
                "unverified boundary: fleet movement predicate requires original traits and live container children"));
    }
    return core::Result<bool>::success(!nonempty || qualifying);
}

FleetControlSetup GalacticSession::Impl::fleet_control_state() const {
    FleetControlSetup result; result.types = setup.fleet_control->types; result.gates = setup.fleet_control->gates;
    for (const auto& [id, row] : detached_units) { (void)id; result.detached_units.push_back(row); }
    return result;
}

void GalacticSession::Impl::execute_fleet(const GameCommand& command, std::uint64_t tick,
    std::vector<GalacticEvent>& events, bool pending_local_conflict, bool production_placement) {
    session_detail::execute_native_fleet(*this, command, tick, events, pending_local_conflict, production_placement);
}

} // namespace eawr::sim::galactic
