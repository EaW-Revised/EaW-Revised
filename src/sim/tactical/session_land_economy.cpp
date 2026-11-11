#include "session_tick.hpp"
#include "session_land_combat_internal.hpp" // include-guard: keep
#include "session_services.hpp"
#include "staging.hpp"
#include "tactical_internal.hpp"
#include "eawr/core/tick_profile.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::tactical::session_detail {
// LC-54..57: the land step runs the shared tactical economy and pad services. Land fills the Tick stage
// optionals the space step opens in open_staging (the seam both steps share), so pad_lifecycle() and economy()
// are the very same functions; the command routes call the same execute_economy as space.
// Inert on land, each with a named TODO row in docs/behaviour/walks/land-combat.md:
//   LE-CAPTURE   pad capture is not serviced (a pad keeps the owner its setup gave it, or the neutral player after
//                its child died);
//   LE-RESPAWN   a respawn batch scheduled by a destroy-when-child-dies pad is never serviced; a land death schedules none;
//   LE-LEVELUP   station level-up upgrades (the space-only station_upgrades phase);
//   LE-BONUS     upgrade combat bonuses (the space-only bonuses phase);
//   LE-OCCUPY    a built child gets no occupancy of its own (the pad's collision stays);
//   LE-ORDER     the economy commands run after the frame's combat, not first in the frame as in space;
//   LE-UNITBUY   a unit option queues and pays into the pool, but land has no deployment route until reinforcement.
core::Result<void> Tick::land_economy(std::vector<LiveUnit>& live, const std::vector<PlayerCommand>& economy_commands,
    std::vector<Event>& events, std::vector<core::Diagnostic>& diagnostics) {
    // Nothing to service: no command, no pad or construction, no income source and every account idle. This is the
    // frame of a land battle that never touches the economy, and it costs a few comparisons.
    // LC-52: a capture point needs the service only on the four-frame capture cadence; a build pad is visited every frame.
    bool pad_work = false;
    if (!impl_->pads.empty()) {
        if ((tick + 1) % 4 == 0) pad_work = true;
        else for (const auto& [id, state] : impl_->pads) {
            const auto unit = std::lower_bound(live.begin(), live.end(), id,
                [](const LiveUnit& row, const EntityId key) { return row.state.entity_id < key; });
            const auto* profile = unit != live.end() && unit->state.entity_id == id
                ? impl_->economy.pads.point(unit->state.type_id) : nullptr;
            if (profile == nullptr || profile->build_pad || state.under_construction != invalid_entity_id
                || state.constructed != invalid_entity_id) { pad_work = true; break; }
        }
    }
    const bool busy = !economy_commands.empty() || pad_work || !impl_->construction.empty() || !impl_->earners.empty()
        || std::any_of(impl_->ledgers.begin(), impl_->ledgers.end(), [](const PlayerEconomy& account) {
            return !account.completed.empty() || !account.income_residuals.empty()
                || std::any_of(account.queues.begin(), account.queues.end(), [](const auto& queue) { return !queue.empty(); });
        });
    if (!busy) return core::Result<void>::success();
    core::tick_profile::Scope profile("serial.tactical.land-economy");
    auto& land = *impl_->land;
    const auto first_new = impl_->next_id;
    const auto live_before = live.size();
    if (!gather_) gather_.emplace();
    if (!gather_->tick_work) gather_->tick_work.emplace();
    if (!gather_->reinforcement_work) gather_->reinforcement_work.emplace();
    if (!staging_) staging_.emplace();
    auto& staging = *staging_;
    if (!staging.arrivals) staging.arrivals.emplace(impl_->arrivals);
    if (!staging.unloaded) staging.unloaded.emplace();
    if (!staging.ledgers) staging.ledgers.emplace(impl_->ledgers);
    if (!staging.crafts) staging.crafts.emplace(impl_->crafts);
    if (!staging.respawns) staging.respawns.emplace(impl_->respawns);
    if (!staging.pad_requests) staging.pad_requests.emplace();
    if (!staging.shares) staging.shares.emplace(impl_->shares);
    if (!staging.arrived_squadrons) staging.arrived_squadrons.emplace();
    if (!staging.earners) staging.earners.emplace(impl_->earners);
    staging.next_id.emplace(impl_->next_id);
    if (!craft_prep_) craft_prep_.emplace();
    if (!craft_prep_->minds) craft_prep_->minds.emplace(impl_->minds);
    if (!tracked_) tracked_.emplace();
    if (!tracked_->production_census_visits) tracked_->production_census_visits.emplace(0);
    if (!impacts_) impacts_.emplace();
    impacts_->events.emplace(std::move(events));
    impacts_->killed.emplace(); // a respawn is never scheduled from a land death (LE-RESPAWN)
    if (!systems_) systems_.emplace();
    systems_->instances.emplace(); // pad_lifecycle writes instance scratch; the land publication rebuilds instances
    if (!surviving_) surviving_.emplace();
    surviving_->survivors.emplace(std::move(live));
    auto& survivors = *surviving_->survivors;
    auto& ledgers = *staging.ledgers;
    auto& arrivals = *staging.arrivals;
    auto& shares = *staging.shares;
    auto& earners = *staging.earners;
    auto& next_id = *staging.next_id;
    auto& crafts = *staging.crafts;
    auto& minds = *craft_prep_->minds;
    auto& pad_requests = *staging.pad_requests;
    auto& arrived_squadrons = *staging.arrived_squadrons;
    auto& placement_work = *gather_->reinforcement_work;
    auto& out_events = *impacts_->events;
    impl_->pad_stage.begin(impl_->pads, impl_->pads.size());
    impl_->construction_stage.begin(impl_->construction, impl_->construction.size() + impl_->pads.size());
    const auto restore_live = [&] {
        live = std::move(survivors);
        events = std::move(out_events);
    };

    if (!economy_commands.empty()) {
        core::tick_profile::Scope phase("land-economy-commands");
        // WPR-33: a buy with a prerequisite or a build limit reads the ownership census, which is recounted before the
        // frame's buys exactly as in space (the census is derived state and is not archived).
        const bool buying = std::any_of(economy_commands.begin(), economy_commands.end(), [&](const PlayerCommand& command) {
            const auto* buy = std::get_if<BuyPayload>(&command.payload);
            if (buy == nullptr) return false;
            return std::any_of(impl_->economy.menus.begin(), impl_->economy.menus.end(), [&](const StationMenu& menu) {
                const auto* option = menu.find(buy->type);
                return option != nullptr && (option->requirements.current_player || option->requirements.current_allies
                    || !option->requirements.prerequisites.empty());
            });
        });
        if (buying) {
            const auto counted = impl_->count_owned(survivors, executor);
            if (!counted) { restore_live(); return core::Result<void>::failure(counted.error()); }
            *tracked_->production_census_visits += survivors.size();
        }
        UnitStage staged(std::move(survivors));
        const auto route = [&](const PlayerCommand& command, const bool executing_pad) -> core::Result<void> {
            Impl::EconomyStage stage{ledgers, arrivals, shares, crafts, minds, arrived_squadrons,
                staging.born_spawners, earners, next_id, impl_->pad_stage.values, impl_->construction_stage.values,
                pad_requests, impl_->pad_stage, impl_->construction_stage, pending_economy_cues_, &placement_work};
            const auto first_added = next_id;
            std::optional<UnitState> sold_before;
            if (std::holds_alternative<PadSellPayload>(command.payload) && !command.units.empty()) {
                const auto sold = staged.find(command.units.front());
                if (sold != staged.end()) sold_before = sold->second.state;
            }
            const auto before_events = out_events.size();
            if (auto executed = impl_->execute_economy(command, tick, staged, stage, out_events, nullptr, executing_pad); !executed)
                return executed;
            if (first_added != next_id && first_added != invalid_entity_id) {
                for (auto id = first_added; id < next_id; ++id) {
                    const auto added = staged.find(id);
                    if (added != staged.end()) track_victory_change(nullptr, &added->second.state, false);
                }
            }
            if (sold_before && out_events.size() > before_events && out_events.back().kind == EventKind::order_accepted
                && std::holds_alternative<PadSellPayload>(command.payload))
                track_victory_change(&*sold_before, nullptr, true);
            if (out_events.size() > before_events && out_events.back().kind == EventKind::order_rejected) {
                diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                    command_context(command.key) + ": " + std::string(to_string(out_events.back().order)) + " rejected ("
                        + std::string(to_string(out_events.back().reason)) + ")", {}, core::Severity::warning));
            }
            return core::Result<void>::success();
        };
        core::Result<void> failure = core::Result<void>::success();
        for (const auto& command : economy_commands) {
            if (auto done = route(command, false); !done) { failure = done; break; }
        }
        // WBP-10: the delayed human pad requests execute after the frame's commands, in request order.
        if (failure) for (std::size_t index = 0; index < pad_requests.size(); ++index) {
            const auto request = pad_requests[index];
            if (auto done = route(request, true); !done) { failure = done; break; }
        }
        survivors = staged.release();
        if (!failure) { restore_live(); return failure; }
    }

    // WBP-16: an under-construction object heals by its increment on every frame after its creation frame, so the
    // completed child inherits the built fraction of its hull. Space does this inside the per-unit systems service;
    // land has no such pass and the construction set is sparse (a few pads), so it is stepped here, before completion.
    for (const auto& [id, building] : impl_->construction_stage.values) {
        auto* unit = live_unit(survivors, id);
        if (unit == nullptr || !unit->durability || tick <= building.start_frame || unit->durability->hull.raw() <= 0) continue;
        const auto* health = impl_->health_profile(*unit);
        if (health == nullptr) continue;
        unit->durability->hull = service_construction(unit->durability->hull, health->max_hull, building);
    }
    // LC-52/53: capture runs before the pad lifecycle, as the space systems pass does.
    if (!impl_->pad_stage.values.empty()) {
        core::tick_profile::Scope phase("land-capture");
        if (auto done = land_capture(survivors, out_events); !done) { restore_live(); return done; }
    }
    if (!impl_->pads.empty() || !impl_->construction.empty()) {
        core::tick_profile::Scope phase("land-pad-lifecycle");
        if (auto done = pad_lifecycle(); !done) { restore_live(); return done; }
    }
    if (!ledgers.empty() || !earners.empty()) {
        core::tick_profile::Scope phase("land-economy-service");
        if (auto done = economy(); !done) { restore_live(); return done; }
    }

    // Registration of the creations and removals this frame made. Only a frame that touched a pad, a
    // construction or a command can have created or removed an object; the sweep is skipped otherwise.
    const bool changed = !economy_commands.empty() || !impl_->pad_stage.dirty.empty() || !impl_->construction_stage.dirty.empty()
        || survivors.size() != live_before || next_id != first_new;
    impl_->pad_stage.commit(impl_->pads);
    impl_->construction_stage.commit(impl_->construction);
    impl_->ledgers = std::move(ledgers);
    impl_->arrivals = std::move(arrivals);
    impl_->shares = std::move(shares);
    impl_->earners = std::move(earners);
    impl_->next_id = next_id;
    staging.respawns->commit();
    staging.crafts->commit();
    craft_prep_->minds->commit();
    if (!pending_productions_.empty()) {
        auto productions = *impl_->productions;
        productions.insert(productions.end(), pending_productions_.begin(), pending_productions_.end());
        impl_->productions = std::make_shared<const std::vector<BattleProduction>>(std::move(productions));
        pending_productions_.clear();
    }
    if (!pending_economy_cues_.empty()) {
        auto cues = *impl_->economy_cues;
        cues.insert(cues.end(), pending_economy_cues_.begin(), pending_economy_cues_.end());
        impl_->economy_cues = std::make_shared<const std::vector<BattleEconomyCue>>(std::move(cues));
        pending_economy_cues_.clear();
    }
    if (changed) {
        const auto present = [&](const EntityId id) {
            const auto found = std::lower_bound(survivors.begin(), survivors.end(), id,
                [](const LiveUnit& unit, const EntityId key) { return unit.state.entity_id < key; });
            return found != survivors.end() && found->state.entity_id == id;
        };
        std::vector<EntityId> removed;
        for (const auto& [id, pose] : land.static_roots) {
            static_cast<void>(pose);
            if (!present(id)) removed.push_back(id);
        }
        for (const auto id : removed) {
            land.static_roots.erase(id);
            land.bindings.erase(id);
            std::erase(land.dynamic_children, id);
        }
        if (!removed.empty() && land_free_occupancy_at_death) {
            std::vector<LandOccupation> occupations;
            bool freed = false;
            for (const auto& occupation : land.occupancy.occupations()) {
                if (present(occupation.object)) occupations.push_back(occupation); else freed = true;
            }
            if (freed) if (auto committed = land.occupancy.commit(occupations, executor); !committed) {
                restore_live();
                return committed;
            }
        }
        // A creation joins the roster as a static root: the built child neither moves nor squads (LE-OCCUPY).
        const auto created = std::lower_bound(survivors.begin(), survivors.end(), first_new,
            [](const LiveUnit& unit, const EntityId key) { return unit.state.entity_id < key; });
        for (auto unit = created; unit != survivors.end(); ++unit) {
            const auto id = unit->state.entity_id;
            if (land.static_roots.contains(id) || land.bindings.contains(id)) continue;
            land.bindings.emplace(id, std::pair{unit->state.type_id, unit->state.owner});
            land.static_roots.emplace(id, unit->state);
            land.dynamic_children.insert(std::upper_bound(land.dynamic_children.begin(), land.dynamic_children.end(), id), id);
        }
    }
    restore_live();
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical::session_detail
