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

core::Result<void> session_detail::Tick::CommandHandlers::repair_hardpoint(const PlayerCommand& command, const RepairHardpointPayload& payload, const PlayerId issuer) const {
    const auto command_tick = phase.tick;
    auto& arrivals = phase.staging_.value().arrivals.value();
    auto& ledgers = phase.staging_.value().ledgers.value();
    auto& staged = phase.tracked_.value().staged.value();
    auto& events = phase.impacts_.value().events.value();
    const auto* repair = &payload;
    auto reason = RejectReason::none;
    const auto found = staged.find(command.units.front());
    if (found == staged.end()) reason = RejectReason::unit_not_live;
    else if (phase.impl_->ledger_of(ledgers, issuer) == nullptr) reason = RejectReason::no_economy;
    else {
        auto& unit = found->second;
        const auto* profile = unit.durability ? phase.impl_->health_profile(unit) : nullptr;
        const bool station = std::any_of(phase.impl_->economy.menus.begin(), phase.impl_->economy.menus.end(),
            [&](const StationMenu& menu) { return menu.station == unit.state.type_id; })
            || std::binary_search(phase.impl_->victory.starbase_types.begin(), phase.impl_->victory.starbase_types.end(), unit.state.type_id);
        if (!station || !profile) reason = RejectReason::not_damageable;
        else if (repair->hardpoint >= profile->hardpoints.size()
            || !profile->hardpoints[repair->hardpoint].destroyable
            || unit.durability->hardpoints[repair->hardpoint].raw() <= 0
            || (unit.durability->hardpoints[repair->hardpoint] >= profile->hardpoints[repair->hardpoint].max_health
                && !hardpoint_disabled(*unit.durability, repair->hardpoint))) {
            reason = RejectReason::hardpoint_invalid;
        } else if (arrivals.contains(unit.state.entity_id)) reason = RejectReason::arriving;
        else {
            // WSL-40/41: service admission has no ownership gate; each payer registers once.
            unit.durability->repairing_players.resize(profile->hardpoints.size());
            auto& payers = unit.durability->repairing_players[repair->hardpoint];
            if (std::find(payers.begin(), payers.end(), issuer) == payers.end()) payers.push_back(issuer);
        }
    }
    events.push_back(Event{command_tick, reason == RejectReason::none ? EventKind::order_accepted : EventKind::order_rejected,
        issuer, command.key.sequence, command.units.front(), OrderKind::repair_hardpoint, reason, static_cast<std::uint8_t>(repair->hardpoint)});
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::CommandHandlers::economy(const PlayerCommand& command) const {
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
    auto& rebuilt = phase.tracked_.value().rebuilt.value();
    auto& tracking = phase.tracked_.value().tracking.value();
    auto& staged = phase.tracked_.value().staged.value();
    auto& events = phase.impacts_.value().events.value();
    auto& pads = phase.impl_->pad_stage.values;
    auto& construction = phase.impl_->construction_stage.values;
    const auto& table = phase.impl_->motion;
    auto& diagnostics = phase.commands_.value().diagnostics.value();
    Impl::EconomyStage stage{ledgers, arrivals, shares, crafts, minds, arrived_squadrons,
        phase.staging_.value().born_spawners, earners, next_id,
        pads, construction, pad_requests, phase.impl_->pad_stage, phase.impl_->construction_stage, phase.pending_economy_cues_, &reinforcement_work};
    CollisionWorld collisions;
    const CollisionWorld* placement_world = nullptr;
    if (tracking && std::holds_alternative<ReinforcePayload>(command.payload)) {
        const auto type = std::get<ReinforcePayload>(command.payload).type;
        const auto* footprint = table.footprint(type);
        auto layer = footprint != nullptr ? footprint->layer : SpaceLayer::none;
        if (layer == SpaceLayer::none && table.squadrons.find_squadron(type)) layer = SpaceLayer::corvette;
        if (const auto selected = dynamic_layer_index(layer)) {
            if (auto built = world_for(*selected, collisions); !built) return core::Result<void>::failure(built.error());
            placement_world = &collisions;
        }
    }
    const auto first_added = next_id;
    std::optional<UnitState> sold_before;
    if (std::holds_alternative<PadSellPayload>(command.payload) && !command.units.empty()) {
        const auto sold = staged.find(command.units.front());
        if (sold != staged.end()) sold_before = sold->second.state;
    }
    if (auto executed = phase.impl_->execute_economy(command, command_tick, staged, stage, events, placement_world); !executed) {
        return core::Result<void>::failure(executed.error());
    }
    if (first_added != next_id && first_added != invalid_entity_id) {
        // Only this command's newly admitted units, never the live world.
        for (auto id = first_added; id < next_id; ++id) {
            const auto added = staged.find(id);
            if (added != staged.end()) {
                phase.track_victory_change(nullptr, &added->second.state, false);
                if (added->second.combat) concentrate_new_bodies.push_back({id, added->second.state.owner, added->second.state.position});
            }
        }
    }
    if (std::holds_alternative<PadSellPayload>(command.payload)
        && events.back().kind == EventKind::order_accepted) {
        // WBP-32/WBF-31: sale uses ordinary elimination evaluation after removal.
        if (sold_before) phase.track_victory_change(&*sold_before, nullptr, true);
        if (tracking) {
            // WBP-32/AV-15: later move commands must plan without the sold obstacle.
            tracking->statics.reset();
            for (auto& layer_view : tracking->views) layer_view.reset();
            rebuilt.fill(true);
            tracking->samples.erase(command.units.front());
        }
    }
    if (events.back().kind == EventKind::order_rejected) {
        diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
            command_context(command.key) + ": " + std::string(to_string(events.back().order)) + " rejected ("
                + std::string(to_string(events.back().reason)) + ")",
            {}, core::Severity::warning));
    }
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
