#include "eawr/sim/galactic/session.hpp"
#include "eawr/core/gc_profile.hpp"


#include "encoding.hpp"
#include "../replay_internal.hpp"
#include "session_impl.hpp"
#include "session_diagnostic.hpp"
#include "checkpoint_fields_internal.hpp"
#include "checkpoint_commands_internal.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "eawr/sim/galactic/economy_corruption.hpp"
#include "eawr/sim/galactic/ability_host.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <limits>
#include <string>
#include <utility>

namespace eawr::sim::galactic {
using session_detail::fail;

using session_detail::FleetData;
using session_detail::GatheredObject;
using session_detail::ObjectKind;
using session_detail::PlanetData;
using session_detail::RouteData;
using session_detail::StableId;

namespace {

// The archive retains authoritative owners and ordered sparse service indexes. Process-local
// adapters, hash caches, immutable content and publication caches are rebound or rebuilt.
template <class A, class T> void checkpoint_owner(A& a, T& v) {
    a(v.completed_tick, v.next_id, v.clock, v.seats, v.seat_control,
        v.economy, v.production, v.income, v.economy_corruption, v.ability_host, v.score_ledger, v.history_archive,
        v.victory, v.control, v.control_effects, v.observer, v.planet_access, v.movement_inputs, v.detached_units,
        v.production_originals, v.actor_records, v.pending_actor_changes, v.population,
        v.population_stale, v.live_object_count, v.applied_outcomes,
        v.pending, v.last_submitted, v.executed,
        v.next_native_invasion, v.native_invasions, v.native_invasion_queue,
        v.native_replay, v.native_replay_cursor, v.native_sequence, v.executed_dropped, v.native_dropped,
        v.native_replay_end, v.native_replaying,
        v.corruption_due, v.active_producers, v.original_owned_counts, v.owned_planet_counts,
        v.income_blocker_next_expiry);
}

// Below this many objects the gather's small copies cost less on one partition than spread over
// the pool (the tactical gather's measured cutoff, #892). Scheduling only: never a result.
constexpr std::size_t gather_single_partition_below = 256;
[[nodiscard]] std::string command_context(const GameCommand& command) {
    return "command (tick " + std::to_string(command.key.tick) + ", seat " + std::to_string(command.key.seat.value)
        + ", sequence " + std::to_string(command.key.sequence) + ")";
}

} // namespace

core::Result<std::vector<std::uint8_t>> GalacticSession::save_checkpoint_state() const {
    using namespace checkpoint_detail;
    try {
        if (impl_->failed || impl_->native_applying) throw Invalid("owner is not at a healthy save barrier");
        Writer out;
        out(impl_->sorted_planets(), impl_->sorted_routes(), impl_->sorted_fleets());
        checkpoint_owner(out, *impl_);
        // The applied journal is history: the recording section carries it, so the owner keeps only its length (#4058).
        // The not yet applied part of a staged recording (native_replay) stays here: no recording carries it.
        out(static_cast<std::uint64_t>(impl_->native_effects.size()));
        out(impl_->snapshot->events());
        return core::Result<std::vector<std::uint8_t>>::success(std::move(out.bytes));
    } catch (const Invalid& error) {
        return fail<std::vector<std::uint8_t>>(diagnostic_codes::invalid_setup, error.what());
    }
}

core::Result<void> GalacticSession::load_checkpoint_state(std::span<const std::uint8_t> bytes) {
    using namespace checkpoint_detail;
    try {
        if (impl_->completed_tick || impl_->failed) throw Invalid("direct restore requires a fresh private owner");
        Reader in(bytes);
        std::vector<Planet> planets; std::vector<Route> routes; std::vector<Fleet> fleets;
        in(planets, routes, fleets);
        // The recording the session was built from staged the applied journal; direct restore adopts it as applied.
        auto journal = std::move(impl_->native_replay);
        impl_->native_replay.clear();
        checkpoint_owner(in, *impl_);
        if (impl_->history_archive.size() != impl_->setup.players.size()) throw Invalid("History archive player roster differs");
        for (const auto& player : impl_->setup.players) {
            const auto at = impl_->history_archive.find(player.player_id);
            if (at == impl_->history_archive.end()) throw Invalid("History archive player is missing");
            for (const auto& row : at->second)
                if (row.frame > galactic_max_ticks || row.planets > galactic_max_objects || row.technology < 0
                    || !row.economy.finite() || !row.military.finite())
                    throw Invalid("invalid stored History field");
        }
        std::uint64_t journal_entries{}; in(journal_entries);
        if (journal_entries != journal.size() || impl_->native_sequence != impl_->native_dropped + journal.size() + 1U
            || (!journal.empty() && (journal.front().sequence != impl_->native_dropped + 1U
                || journal.back().frontier > impl_->completed_tick)))
            throw Invalid("owner journal differs from the recording it restores");
        impl_->native_effects = std::move(journal);
        std::vector<GalacticEvent> events; in(events);
        if (in.remaining() || impl_->completed_tick > galactic_max_ticks
            || !impl_->next_id
            || impl_->applied_outcomes > impl_->setup.outcomes.size()
            || impl_->native_replay_cursor > impl_->native_replay.size()) throw Invalid("inconsistent owner barrier");
        std::set<EntityId> ids;
        const auto identity = [&](EntityId id) {
            if (!id || id >= impl_->next_id || !ids.insert(id).second) throw Invalid("invalid live object identity");
        };
        impl_->registry.clear(); impl_->planets.clear(); impl_->routes.clear(); impl_->fleets.clear();
        for (const auto& row : planets) { identity(row.id); impl_->insert(row); }
        for (const auto& row : routes) { identity(row.id); impl_->insert(row); }
        for (const auto& row : fleets) {
            if (row.raid_destination && (!*row.raid_destination || !impl_->movement_inputs || !impl_->movement_inputs->raid
                || !impl_->planets.contains(*row.raid_destination))) throw Invalid("invalid retained raid destination");
            identity(row.id); impl_->insert(row);
        }
        if (impl_->setup.rules.maximum_fleet_movement_distance) {
            auto graph = PlanetGraph::build(planets, routes, *impl_->setup.rules.maximum_fleet_movement_distance);
            if (!graph) return core::Result<void>::failure(graph.error());
            impl_->graph = std::move(graph).value();
        }
        impl_->native_invasion_transport_count = 0;
        for (const auto id : impl_->native_invasion_queue) {
            const auto at = impl_->native_invasions.find(id);
            if (at == impl_->native_invasions.end()) throw Invalid("invasion queue names an absent request");
            impl_->native_invasion_transport_count += at->second.transports.size();
        }
        impl_->rebuild_technology_price_expiry();
        impl_->published_actor_records.reset(); impl_->published_production_originals.reset();
        impl_->production_offer_hashes.reset(); impl_->view_publications.clear();
        impl_->published_economy_input_generation.reset();
        impl_->income_forecast_cache.reset(); impl_->gathered_income_inputs.reset();
        impl_->movement_access_generation = std::numeric_limits<std::uint64_t>::max();
        impl_->control_stealth_inputs.clear(); impl_->control_stealth_generation = 0;
        impl_->dirty.fill(true); impl_->cached_hash_present = false;
        impl_->invalidate_observer_projection();
        impl_->snapshot = impl_->make_snapshot(std::move(events));
        auto canonical = save_checkpoint_state();
        if (!canonical || !std::equal(bytes.begin(), bytes.end(), canonical.value().begin(), canonical.value().end()))
            throw Invalid("noncanonical owner barrier");
        return core::Result<void>::success();
    } catch (const Invalid& error) {
        return fail<void>(diagnostic_codes::invalid_setup, error.what());
    }
}

const Seat* GalacticSession::Impl::seat(const SeatId id) const noexcept {
    const auto found = std::lower_bound(
        seats.begin(), seats.end(), id, [](const Seat& row, const SeatId value) { return row.seat_id < value; });
    return found != seats.end() && found->seat_id == id ? &*found : nullptr;
}

void GalacticSession::Impl::execute(const GameCommand& command, const std::uint64_t tick, std::vector<GalacticEvent>& events,
    const bool pending_local_conflict) {
    GalacticEvent event{tick, GalacticEventKind::command_executed, command.key.seat, invalid_entity_id,
        command.key.sequence, 0, GalacticRejectReason::none};
    const auto reject = [&](const GalacticRejectReason reason) {
        event.kind = GalacticEventKind::command_rejected;
        event.reason = reason;
        events.push_back(event);
    };
    const auto* issuer = seat(command.key.seat);
    const auto session_dirty = [&] { dirty[static_cast<std::size_t>(StateSection::session)] = true; };
    if (std::holds_alternative<SeatLeave>(command.payload) || std::holds_alternative<SeatRejoin>(command.payload)) {
        if (!seat_control || !issuer || issuer->kind != SeatKind::human)
            return reject(GalacticRejectReason::not_authorized);
        const bool active = std::holds_alternative<SeatRejoin>(command.payload);
        if (issuer->active == active) return reject(GalacticRejectReason::inactive_seat);
        const auto index = static_cast<std::size_t>(issuer - seats.data());
        seats[index].active = active;
        if (!active && multiplayer_flag() && std::none_of(seats.begin(), seats.end(), [](const auto& row) {
            return row.active && row.kind == SeatKind::human;
        })) clock.pause_depth = pending_local_conflict ? std::min(clock.pause_depth, 1U) : 0U;
        update_seat_control_policy(*seat_control, seats);
        // Only the departing/rejoining faction's fallback issuers change activity.
        for (auto& row : seats) if (row.kind == SeatKind::ai && row.player == issuer->player) {
            const auto& controllers = seat_control->control.controllers;
            const auto controller = std::find_if(controllers.begin(), controllers.end(), [&](const auto& value) { return value.player == row.player; });
            row.active = controller != controllers.end() && controller->kind == FactionControl::ai;
        }
        events.push_back(event);
        // TM-04: fast forward is single-human; a rejoin that makes the session multi-human ends it here, in order.
        if (active && clock.strategic_speed.raw() == fast_forward_factor_raw
            && std::count_if(seats.begin(), seats.end(), [](const Seat& row) { return row.active && row.kind == SeatKind::human; }) > 1) {
            clock.strategic_speed = math::Fixed::from_raw(math::Fixed::scale);
            event.kind = GalacticEventKind::speed_changed; event.subject = 0U; event.value = static_cast<std::uint64_t>(clock.strategic_speed.raw());
            events.push_back(event);
        }
        session_dirty(); return;
    }
    if (const auto* role = std::get_if<AssignBattleTeamRole>(&command.payload)) {
        if (!seat_control || !seat_control->battle || seat_control->battle->conflict != role->conflict)
            return reject(GalacticRejectReason::unknown_subject);
        if (!issuer || !issuer->active) return reject(GalacticRejectReason::inactive_seat);
        auto& battle = *seat_control->battle;
        const auto& teams = seat_control->control.teams;
        const auto team = std::find_if(teams.begin(), teams.end(), [&](const auto& value) { return value.side == battle.choosing_side; });
        if (issuer->kind != SeatKind::human || team == teams.end() || team->lead != issuer->seat_id
            || issuer->human_side_id != battle.choosing_side || battle.role != BattleTeamRole::pending)
            return reject(GalacticRejectReason::not_authorized);
        battle.role = role->role; event.subject = battle.conflict; event.value = static_cast<std::uint64_t>(battle.role);
        session_dirty(); events.push_back(event); return;
    }
    if (const auto* fixup = std::get_if<PostLoadFixup>(&command.payload)) {
        clock.pause_depth = fixup->resume ? 1U : fixup->pause_depth;
        // WGS-61: the single-player zero-depth load uses the normal resume path.
        if (fixup->resume) execute({command.key, ResumeGalaxy{}}, tick, events);
        session_dirty();
        event.kind = GalacticEventKind::load_fixup_completed;
        event.subject = fixup->pending_battle ? 1U : 0U;
        event.value = fixup->autosave ? 1U : 0U;
        events.push_back(event);
        return;
    }
    if (const auto* deployment = std::get_if<DeployGroundAbility>(&command.payload)) {
        const auto before = events.size();
        dispatch_ground_ability(deployment->actor, deployment->planet, command.key.seat, command.key.sequence, tick, events);
        if (events.size() == before) { event.kind = GalacticEventKind::command_rejected; event.reason = GalacticRejectReason::unknown_value; events.push_back(event); }
        return;
    }
    if (std::holds_alternative<ObserverCommand>(command.payload)) {
        execute_observer(command, tick, events); return;
    }
    if (std::holds_alternative<PrepareBlackMarket>(command.payload) || std::holds_alternative<BuyBlackMarket>(command.payload)) {
        if (ability_host) { execute_ability(command, tick, events); return; }
        execute_black_market(command, tick, events); return;
    }
    if (std::holds_alternative<PrepareGalacticAbility>(command.payload)
        || std::holds_alternative<ExecutePreparedAbility>(command.payload)
        || std::holds_alternative<CancelGalacticAbility>(command.payload)) {
        execute_ability(command, tick, events); return;
    }
    if (const auto* save = std::get_if<SaveRequest>(&command.payload)) {
        const bool allowed = issuer && issuer->active && issuer->kind == SeatKind::human
            // WGS-33: quick-save and autosave do nothing in multiplayer mode; the ordinary save stays admitted.
            && (!(save->quick || save->autosave) || !multiplayer_flag())
            // WGS-99/142/143: a prebattle autosave is the producer at Begin, issued from the conflict's own
            // pause with the conflict still pending and no child staged; every other state refuses it.
            && (!save->prebattle || prebattle_autosave_window);
        event.kind = allowed ? GalacticEventKind::save_requested : GalacticEventKind::save_rejected;
        event.value = static_cast<std::uint64_t>(save->slot);
        event.subject = save->prebattle ? 1U : 0U;
        events.push_back(event);
        return;
    }
    if (std::holds_alternative<PauseGalaxy>(command.payload) || std::holds_alternative<ResumeGalaxy>(command.payload)
        || std::holds_alternative<SetSpeed>(command.payload)) {
        // Any active seat, in coop and versus alike, until the multiplayer vote rules land.
        if (issuer == nullptr || !issuer->active) return reject(GalacticRejectReason::inactive_seat);
    }
    // TP-MP02: one shared human hold; system conflict/summary holds are separate.
    const std::uint32_t protected_pause = pending_local_conflict ? 1U : 0U;
    if (multiplayer_flag() && (std::holds_alternative<PauseGalaxy>(command.payload)
        || std::holds_alternative<ResumeGalaxy>(command.payload)) && issuer->kind != SeatKind::human)
        return reject(GalacticRejectReason::not_authorized);
    if (std::holds_alternative<PauseGalaxy>(command.payload)) {
        if (multiplayer_flag() && clock.pause_depth > protected_pause) { events.push_back(event); return; }
        events.push_back(event);
        // WGM-05: nested; only the first pause has first-pause effects.
        ++clock.pause_depth;
        session_dirty();
        if (clock.pause_depth == 1 || (multiplayer_flag() && clock.pause_depth == protected_pause + 1)) {
            event.kind = GalacticEventKind::galaxy_paused;
            events.push_back(event);
        }
        return;
    }
    if (std::holds_alternative<ResumeGalaxy>(command.payload)) {
        // WGM-06: a resume needs a positive counter; only the decrement to zero resumes.
        if (clock.pause_depth == 0 || (multiplayer_flag() && clock.pause_depth <= protected_pause))
            return reject(GalacticRejectReason::not_paused);
        events.push_back(event);
        if (multiplayer_flag()) clock.pause_depth = protected_pause;
        else --clock.pause_depth;
        session_dirty();
        if (clock.pause_depth == 0 || (multiplayer_flag() && clock.pause_depth == protected_pause)) {
            event.kind = GalacticEventKind::galaxy_resumed;
            events.push_back(event);
        }
        return;
    }
    if (const auto* speed = std::get_if<SetSpeed>(&command.payload)) {
        // TM-04: fast forward is a single-human feature; with several active human seats the
        // strategic fast-forward factor is refused here, not only hidden by the HUD.
        if (!speed->tactical && speed->factor.raw() == fast_forward_factor_raw
            && std::count_if(seats.begin(), seats.end(), [](const Seat& row) { return row.active && row.kind == SeatKind::human; }) > 1)
            return reject(GalacticRejectReason::not_authorized);
        event.subject = speed->tactical ? 1U : 0U;
        event.value = static_cast<std::uint64_t>(speed->factor.raw());
        events.push_back(event);
        auto& setting = speed->tactical ? clock.tactical_speed : clock.strategic_speed;
        if (setting == speed->factor) return;
        setting = speed->factor;
        event.kind = GalacticEventKind::speed_changed;
        events.push_back(event);
        return;
    }
    if (const auto* order = std::get_if<DebugSetPlanetOwner>(&command.payload)) {
        event.subject = order->planet;
        event.value = order->owner;
        if (issuer == nullptr || !issuer->active) return reject(GalacticRejectReason::inactive_seat);
        const auto planet = planets.find(order->planet);
        const auto& players = setup.players;
        if (planet == planets.end()) return reject(GalacticRejectReason::unknown_subject);
        if (std::none_of(players.begin(), players.end(), [&](const GalacticPlayer& row) { return row.player_id == order->owner; })) {
            return reject(GalacticRejectReason::unknown_value);
        }
        events.push_back(event);
        auto& data = registry.get<PlanetData>(planet->second);
        if (data.owner == order->owner) return;
        --owned_planet_counts[data.owner];
        ++owned_planet_counts[order->owner];
        record_planet_change(order->planet, data.owner, order->owner); // WGP-101
        const auto previous_owner = data.owner;
        data.owner = order->owner;
        if (auto refreshed = refresh_planet_technology_price(order->planet, true); !refreshed) { failed = true; return; }
        transfer_planet_spies(order->planet, previous_owner, data.owner);
        invalidate_income_inputs();
        relocate_actor(order->planet, data.owner, order->planet, 0);
        population_stale = true;
        refresh_credit_caps();
        dirty[static_cast<std::size_t>(StateSection::planets)] = true;
        event.kind = GalacticEventKind::planet_owner_changed;
        events.push_back(event);
        return;
    }
    if (const auto* order = std::get_if<DebugSetFleetOrbit>(&command.payload)) {
        event.subject = order->fleet;
        event.value = order->planet;
        if (issuer == nullptr || !issuer->active) return reject(GalacticRejectReason::inactive_seat);
        const auto fleet = fleets.find(order->fleet);
        if (fleet == fleets.end()) return reject(GalacticRejectReason::unknown_subject);
        auto& data = registry.get<FleetData>(fleet->second);
        // ADR-020: a seat commands what its player owns; the seat itself never owns anything.
        if (data.owner != issuer->player) return reject(GalacticRejectReason::not_authorized);
        if (!planets.contains(order->planet)) return reject(GalacticRejectReason::unknown_value);
        events.push_back(event);
        if (data.orbit == order->planet) return;
        data.orbit = order->planet;
        relocate_actor(order->fleet, data.owner, data.orbit, data.orbit);
        for (const auto& member : data.members) relocate_actor(member.id, data.owner, data.orbit, order->fleet);
        population_stale = true;
        if (setup.movement) {
            data.motion.transit.reset();
            std::uint64_t latest = 0;
            for (const auto& [id, handle] : fleets) {
                (void)id; latest = std::max(latest, registry.get<FleetData>(handle).motion.orbit_entry);
            }
            data.motion.orbit_entry = latest + 1;
            dirty[static_cast<std::size_t>(StateSection::movement)] = true;
        }
        dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
        invalidate_observer_projection();
        event.kind = GalacticEventKind::fleet_orbit_changed;
        events.push_back(event);
        return;
    }
    if (std::holds_alternative<LandFleet>(command.payload)) {
        execute_landing(command, tick, events, pending_local_conflict); return;
    }
    if (std::holds_alternative<LaunchFromSurface>(command.payload)) {
        // WGM-13 (#2880): the seat launch reuses the native launch gates and ordinary fleet admission.
        execute_launch(command, tick, events, pending_local_conflict); return;
    }
    if (std::holds_alternative<LaunchFleet>(command.payload)) {
        execute_launch_fleet(command, tick, events, pending_local_conflict); return;
    }
    if (const auto* transfer = std::get_if<TransferSurface>(&command.payload)) {
        execute_launch_fleet({command.key, LaunchFleet{transfer->units, transfer->path, {}, 0}},
            tick, events, pending_local_conflict, true); return;
    }
    if (const auto* order = std::get_if<RecheckOrbit>(&command.payload)) {
        event.subject = order->target;
        if (!issuer || issuer->kind != SeatKind::script || !issuer->active) return reject(GalacticRejectReason::not_authorized);
        if (auto tested = recheck_movement_orbit(order->target, tick, events); !tested)
            return reject(GalacticRejectReason::unknown_subject);
        events.push_back(event); return;
    }
    if (std::holds_alternative<MoveFleet>(command.payload)) {
        execute_movement(command, tick, events, pending_local_conflict);
        if (dirty[static_cast<std::size_t>(StateSection::fleets)]) population_stale = true;
        return;
    }
    if (layer_of(command.payload) == CommandLayer::galactic) {
        if (std::holds_alternative<SellObject>(command.payload)) {
            execute_object_sale(command, tick, events); return;
        }
        if (std::holds_alternative<ProductionCommand>(command.payload)) {
            execute_production(command, tick, events);
            return;
        }
        execute_fleet(command, tick, events, pending_local_conflict);
        return;
    }
    // Tactical-layer commands never reach the galaxy: submit() refuses them.
    reject(GalacticRejectReason::unknown_subject);
}

core::Result<void> GalacticSession::Impl::journal_native_receipt(GalacticSession& world,
    NativeEffectKind kind, PlayerId player, EntityId actor, EntityId target, bool conflict_pending,
    const CommandPayload* command, const NativeFleetReceipt& receipt) {
    if (native_applying) return core::Result<void>::success();
    std::vector<std::uint8_t> payload;
    detail::put_u32(payload, player); detail::put_u64(payload, actor); detail::put_u64(payload, target);
    // Reconstruction can run before the campaign summary owner is restored. Preserve the effective
    // admission gate in the existing journal input, including a summary-only system pause.
    const bool admission_held = conflict_pending || (system_hold && clock.pause_depth && kind != NativeEffectKind::orbit_recheck);
    detail::put_u8(payload, admission_held ? 1 : 0); detail::put_u16(payload, command ? opcode_of(*command) : 0);
    const auto body = command ? encode_payload(*command) : std::vector<std::uint8_t>{};
    detail::put_u32(payload, static_cast<std::uint32_t>(body.size())); detail::put_bytes(payload, body);
    detail::put_u8(payload, receipt.admitted ? 1 : 0); detail::put_u64(payload, receipt.object);
    detail::put_u16(payload, static_cast<std::uint16_t>(receipt.reason));
    return world.append_native_effect(kind, std::move(payload));
}

core::Result<void> GalacticSession::reserve_native_identity_frontier(EntityId expected, EntityId next) {
    return reserve_native_frontiers(expected, next, impl_->next_native_invasion, impl_->next_native_invasion);
}
core::Result<void> GalacticSession::reserve_native_frontiers(EntityId expected, EntityId next,
    std::uint64_t expected_invasion, std::uint64_t next_invasion) {
    auto& impl = *impl_;
    if (impl.failed || native_shadow() || expected != impl.next_id || next < expected
        || next == std::numeric_limits<EntityId>::max() || !expected_invasion
        || expected_invasion != impl.next_native_invasion || next_invasion < expected_invasion
        || next_invasion == std::numeric_limits<std::uint64_t>::max())
        return fail<void>(diagnostic_codes::invalid_setup, "native identity reservation requires its current ordered frontiers");
    if (next == expected && next_invasion == expected_invasion) return core::Result<void>::success();
    std::vector<std::uint8_t> payload;
    detail::put_u64(payload, expected); detail::put_u64(payload, next);
    detail::put_u64(payload, expected_invasion); detail::put_u64(payload, next_invasion);
    if (auto recorded = append_native_effect(NativeEffectKind::identity_reservation, std::move(payload)); !recorded)
        return recorded;
    if (next != expected) impl.dirty[static_cast<std::size_t>(StateSection::session)] = true;
    if (next_invasion != expected_invasion) impl.dirty[static_cast<std::size_t>(StateSection::native_invasions)] = true;
    impl.next_id = next; impl.next_native_invasion = next_invasion;
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::append_native_effect(NativeEffectKind kind, std::vector<std::uint8_t> payload,
    NativeEffectPhase phase) {
    auto& impl = *impl_;
    if (impl.native_applying) return core::Result<void>::success();
    if (auto* shadow = native_shadow()) return stage_native_effect(*shadow, kind, std::move(payload), phase);
    if (impl.failed || impl.native_replaying || kind < NativeEffectKind::fleet || kind > NativeEffectKind::identity_reservation
        || phase > NativeEffectPhase::world_service || payload.size() > galactic_replay_max_payload
        || impl.native_dropped + impl.native_effects.size() + impl.executed_dropped + impl.executed.size() + impl.pending.size() >= galactic_max_commands
        || impl.native_sequence == std::numeric_limits<std::uint64_t>::max()) {
        impl.failed = true;
        return fail<void>(diagnostic_codes::invalid_command, "native journal requires a live ordered owner and bounded payload");
    }
    if (!impl.native_effects.empty()) {
        const auto& last = impl.native_effects.back();
        if (impl.completed_tick < last.frontier || (impl.completed_tick == last.frontier && phase < last.phase)) {
            impl.failed = true; return fail<void>(diagnostic_codes::command_order, "native journal phase rewound");
        }
    }
    impl.native_effects.push_back({impl.completed_tick, impl.native_sequence++, phase, kind, std::move(payload)});
    return core::Result<void>::success();
}
void GalacticSession::set_native_effect_replayer(NativeEffectKind kind, NativeEffectReplayer replay) {
    if (replay) impl_->native_replayers[kind] = std::move(replay); else impl_->native_replayers.erase(kind);
}
core::Result<void> GalacticSession::stage_native_replay(std::span<const NativeEffect> effects, std::uint64_t final_tick) {
    return stage_native_replay(effects, final_tick, 0);
}
core::Result<void> GalacticSession::stage_native_replay(std::span<const NativeEffect> effects, std::uint64_t final_tick,
    std::uint64_t sequence_base) {
    auto& impl = *impl_;
    // A fresh world adopts a recording window that starts at sequence_base + 1 (#4058); a rebased world takes the
    // window that continues its own journal. Either way the effects arrive numbered from 1 within the window.
    if (impl.failed || !impl.native_effects.empty() || impl.native_replaying
        || (impl.completed_tick && sequence_base != impl.native_dropped))
        return fail<void>(diagnostic_codes::invalid_setup, "native reconstruction requires a fresh or rebased world");
    GalacticReplay probe{impl.setup, final_tick, {}, std::vector<NativeEffect>(effects.begin(), effects.end())};
    if (auto valid = validate_replay(probe); !valid) return valid;
    for (auto& effect : probe.native_effects) effect.sequence += sequence_base;
    impl.native_replay = std::move(probe.native_effects); impl.native_replay_cursor = 0;
    impl.native_replay_end = final_tick; impl.native_replaying = !impl.native_replay.empty();
    return core::Result<void>::success();
}
bool GalacticSession::replaying_native_effects() const noexcept { return impl_->native_replaying; }
std::span<const NativeEffect> GalacticSession::native_effects() const noexcept { return impl_->native_effects; }
void GalacticSession::rebase_history() noexcept {
    auto& impl = *impl_;
    // Every executed command ran before the boundary, and the applied journal is in the saved window (#4058).
    impl.executed_dropped += impl.executed.size(); impl.executed.clear();
    impl.native_dropped += impl.native_effects.size(); impl.native_effects.clear();
}
std::uint64_t GalacticSession::executed_dropped() const noexcept { return impl_->executed_dropped; }
std::uint64_t GalacticSession::native_dropped() const noexcept { return impl_->native_dropped; }
core::Result<void> GalacticSession::finish_native_replay() {
    auto& impl = *impl_;
    if (impl.native_replay_cursor != impl.native_replay.size())
        return fail<void>(diagnostic_codes::invalid_setup, "native reconstruction left unapplied effects");
    impl.native_replaying = false; impl.native_replay.clear(); impl.native_replay_cursor = 0;
    return core::Result<void>::success();
}
core::Result<std::vector<std::uint8_t>> GalacticSession::consume_native_effect(NativeEffectKind kind) {
    using Result = core::Result<std::vector<std::uint8_t>>;
    auto& impl = *impl_;
    if (!impl.native_replaying || impl.native_replay_cursor >= impl.native_replay.size())
        return fail<std::vector<std::uint8_t>>(diagnostic_codes::invalid_setup, "native notice has no recorded outcome");
    const auto& effect = impl.native_replay[impl.native_replay_cursor];
    if (effect.frontier != impl.completed_tick || effect.phase != NativeEffectPhase::world_service || effect.kind != kind)
        return fail<std::vector<std::uint8_t>>(diagnostic_codes::invalid_setup, "native notice differs from its recorded owner order");
    impl.native_effects.push_back(effect); impl.native_sequence = effect.sequence + 1; ++impl.native_replay_cursor;
    return Result::success(effect.payload);
}
core::Result<void> GalacticSession::replay_native_barrier(const PartitionExecutor& executor) {
    auto& impl = *impl_;
    if (!impl.native_replaying) return core::Result<void>::success();
    while (impl.native_replay_cursor < impl.native_replay.size()) {
        const auto& effect = impl.native_replay[impl.native_replay_cursor];
        if (effect.frontier > impl.completed_tick || (effect.frontier == impl.completed_tick && effect.phase != NativeEffectPhase::barrier)) break;
        if (effect.frontier != impl.completed_tick || effect.phase != NativeEffectPhase::barrier)
            return fail<void>(diagnostic_codes::invalid_setup, "native reconstruction missed its recorded phase");
        const auto handler = impl.native_replayers.find(effect.kind);
        if (effect.kind != NativeEffectKind::identity_reservation && handler == impl.native_replayers.end())
            return fail<void>(diagnostic_codes::invalid_setup, "native effect requires its typed reconstruction owner");
        struct Applying {
            bool& flag;
            explicit Applying(bool& value) : flag(value) { flag = true; }
            ~Applying() { flag = false; }
        } applying(impl.native_applying);
        auto applied = [&]() -> core::Result<void> {
            if (effect.kind != NativeEffectKind::identity_reservation) return handler->second(*this, effect, executor);
            if (effect.payload.size() != 32)
                return fail<void>(diagnostic_codes::invalid_setup, "native identity reservation payload is invalid");
            const auto read = [&](std::size_t offset) {
                std::uint64_t value{};
                for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t{effect.payload[offset + i]} << (i * 8);
                return value;
            };
            return reserve_native_frontiers(read(0), read(8), read(16), read(24));
        }();
        if (!applied) { impl.failed = true; return applied; }
        impl.native_effects.push_back(effect); impl.native_sequence = effect.sequence + 1; ++impl.native_replay_cursor;
    }
    if (impl.completed_tick >= impl.native_replay_end && impl.native_replay_cursor == impl.native_replay.size())
        return finish_native_replay();
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::submit(const GameCommand& command) {
    if (std::holds_alternative<PostLoadFixup>(command.payload))
        return fail<void>(diagnostic_codes::invalid_command, "load fixups are internal restore commands");
    const auto context = command_context(command);
    const auto& key = command.key;
    if (impl_->seat(key.seat) == nullptr) return fail<void>(diagnostic_codes::unknown_issuer, context + ": no such seat");
    const auto layer = layer_of(command.payload);
    if (layer == CommandLayer::tactical) {
        return fail<void>(diagnostic_codes::invalid_command, context + ": a tactical command goes to its battle, not the galaxy");
    }
    const auto opcode = opcode_of(command.payload);
    if (decode_payload(layer, opcode, encode_payload(command.payload)) != std::optional(command.payload)) {
        return fail<void>(diagnostic_codes::invalid_command, context + ": malformed payload");
    }
    const bool debug = layer == CommandLayer::session
        && (opcode == session_opcodes::debug_set_planet_owner || opcode == session_opcodes::debug_set_fleet_orbit);
    if (debug && !impl_->setup.debug_commands) {
        return fail<void>(diagnostic_codes::invalid_command, context + ": debug commands are off in this session");
    }
    if (key.tick >= galactic_max_ticks) {
        return fail<void>(diagnostic_codes::resource_limit, context + ": tick exceeds the galactic tick limit");
    }
    if (key.tick < impl_->completed_tick) {
        return fail<void>(diagnostic_codes::late_command, context + ": tick " + std::to_string(key.tick)
                + " has already executed; the next tick to execute is " + std::to_string(impl_->completed_tick));
    }
    const std::pair order{key.tick, key.sequence};
    const auto previous = impl_->last_submitted.find(key.seat);
    if (previous != impl_->last_submitted.end() && !(previous->second < order)) {
        return fail<void>(diagnostic_codes::command_order, context + ": does not follow the seat's previous command (tick "
                + std::to_string(previous->second.first) + ", sequence " + std::to_string(previous->second.second) + ")");
    }
    if (impl_->executed_dropped + impl_->executed.size() + impl_->pending.size() >= galactic_max_commands) {
        return fail<void>(diagnostic_codes::resource_limit, context + ": session command count limit reached");
    }
    impl_->pending.emplace(key, command);
    impl_->last_submitted.insert_or_assign(key.seat, order);
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::Impl::service_tick_control(GalacticSession& session, const PartitionExecutor& executor,
    const std::uint64_t executing, std::vector<GalacticEvent>& events, GalacticTick& tick) {
    auto& impl = *this;
    // GFD-02/05: committed ownership listeners and immediate credit-cap clamp
    // precede fiscal; its consumer must gather fresh post-control inputs.
    // GFD-01 / GFS-U01 retain arrival-to-control admission as a scheduling seam.
    if (auto completed = impl.service_corruption(executor, events.size()); !completed) {
        impl.failed = true; return core::Result<void>::failure(completed.error());
    }
    if (auto controlled = impl.service_control(executor, executing, events, tick.work); !controlled) {
        impl.failed = true;
        return core::Result<void>::failure(controlled.error());
    }
    if (auto access = session.refresh_movement_access(executor); !access) {
        impl.failed = true; return core::Result<void>::failure(access.error());
    }
    if (impl.control_handler && !impl.control_effects.empty()) {
        const auto current_planets = impl.sorted_planets();
        auto applied = impl.control_handler(impl.control_effects, current_planets);
        if (!applied) { impl.failed = true; return core::Result<void>::failure(applied.error()); }
    }
    // WGM-75/76: conquest listeners see the committed owner and clamped
    // caps before the first-capture windfall. History was recorded by control.
    if (impl.economy) for (const auto& effect : impl.control_effects) {
        if (!effect.first_capture) continue;
        auto& accounts = impl.economy->players;
        const auto account = std::lower_bound(accounts.begin(), accounts.end(), effect.new_owner,
            [](const auto& row, PlayerId player) { return row.player < player; });
        if (account == accounts.end() || account->player != effect.new_owner) continue;
        const auto multiplier = impl.setup.fiscal ? impl.setup.fiscal->players[
            static_cast<std::size_t>(account - accounts.begin())].positive_credit_multiplier
            : math::Fixed::from_raw(math::Fixed::scale);
        auto balance = adjusted_credits(account->credits, effect.capture_reward, multiplier, account->credit_cap);
        if (!balance) { impl.failed = true; return core::Result<void>::failure(balance.error()); }
        auto delta = math::subtract(balance.value(), account->credits);
        if (!delta) { impl.failed = true; return core::Result<void>::failure(delta.error()); }
        if (impl.credit_notification_handler) {
            auto notified = impl.credit_notification_handler({CreditNotification::Kind::windfall,
                effect.new_owner, balance.value(), delta.value(), {}});
            if (!notified) { impl.failed = true; return core::Result<void>::failure(notified.error()); }
        }
        account->credits = balance.value();
        impl.dirty[static_cast<std::size_t>(StateSection::players)] = true;
        // WGP-53: first-capture award, after crediting; not every windfall.
        if (impl.feedback_notice_handler && impl.observer && std::binary_search(impl.observer->human_observers.begin(), impl.observer->human_observers.end(), effect.new_owner))
            impl.feedback_notice_handler({GalacticFeedbackNotice::Kind::capture_reward, effect.new_owner, effect.planet, events.size()});
        if (auto tested = impl.test_credit_condition(effect.new_owner, executing, events); !tested) {
            impl.failed = true; return core::Result<void>::failure(tested.error());
        }
    }
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::Impl::service_tick_objects(GalacticSession& session, const PartitionExecutor& executor,
    const std::uint64_t executing, std::vector<GalacticEvent>& events, GalacticTick& tick,
    const bool pending_local_conflict) {
    auto& impl = *this;
    tick.work.serviced = true;
    // galactic-gather: every object's state copied into its ascending-ID slot, the immutable
    // input of the per-object phases that follow it (galactic architecture, section 10).
    std::optional<gc_profile::Scope> stage_scope(std::in_place, gc_profile::Phase::tick_gather);
    const auto objects = impl.sorted_objects();
    std::vector<GatheredObject> gathered(objects.size());
    const auto partitions = objects.size() < gather_single_partition_below ? std::size_t{1} : tick_partition_count;
    const auto phase = executor.execute_phase("galactic-gather", partitions, [&](const std::size_t partition) {
        const auto range = partitions == 1 ? PartitionRange{0, objects.size()} : partition_range(partition, objects.size());
        for (auto index = range.begin; index < range.end; ++index) {
            gathered[index] = impl.gather(objects[index].first, objects[index].second);
        }
    });
    if (!phase) {
        impl.failed = true;
        return fail<void>(diagnostic_codes::worker_failure, "galactic-gather failed: " + phase.error().message);
    }
    tick.work.gathered_objects = gathered.size();
    stage_scope.emplace(gc_profile::Phase::tick_movement);
    // WGM-89: movement object service precedes fiscal processing.
    if (auto access = session.refresh_movement_access(executor); !access) {
        impl.failed = true; return core::Result<void>::failure(access.error());
    }
    if (auto moved = impl.service_movement(gathered, executor, executing, events); !moved) {
        impl.failed = true;
        return core::Result<void>::failure(moved.error());
    }
    if (impl.dirty[static_cast<std::size_t>(StateSection::fleets)]) impl.population_stale = true;
    stage_scope.emplace(gc_profile::Phase::tick_control);

    if (auto result = impl.service_tick_control(session, executor, executing, events, tick); !result)
        return core::Result<void>::failure(result.error());
    // GFD-03/08: command purchases precede collection; production placement
    // commits before fresh fiscal reads. GFD-04 keeps all reads before timer advance.
    // GFS-19/GFC-09: object ability service precedes fiscal.
    // income_production_turn_order: gambling sample before production is
    // placed in the GFS-19 ability turn; RNG/source lifecycle remains GFS-U05.
    // Its order against production completion is unqualified.
    stage_scope.emplace(gc_profile::Phase::tick_income);
    if (auto serviced = impl.service_income(executor, executing, events); !serviced) {
        impl.failed = true;
        return fail<void>(diagnostic_codes::worker_failure, serviced.error().message);
    }
    stage_scope.emplace(gc_profile::Phase::tick_production);
    if (auto serviced = impl.service_production(executor, executing, events, pending_local_conflict); !serviced) {
        impl.failed = true;
        return core::Result<void>::failure(serviced.error());
    }
    tick.work.production_producers = impl.production_producers;
    tick.work.production_entries = impl.production_entries;
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::Impl::service_tick_fiscal(const PartitionExecutor& executor, const std::uint64_t executing,
    std::vector<GalacticEvent>& events, GalacticTick& tick, const std::uint32_t duration) {
    auto& impl = *this;
    auto& tick_clock = impl.clock;
    gc_profile::Scope fiscal_scope(gc_profile::Phase::tick_fiscal);
    if (impl.setup.fiscal) {
        const auto& fiscal = *impl.setup.fiscal;
        // GFD-02/05/08: current owners and sources after political/production commits.
        const auto income_planets = impl.sorted_planets();
        auto contributions = impl.income_contributions(executor);
        if (!contributions) {
            impl.failed = true;
            return fail<void>(diagnostic_codes::worker_failure, contributions.error().message);
        }
        // WGEP-75: only actual fiscal evaluation commits the retained source.
        if (impl.control && fiscal.contributions) for (std::size_t i = 0; i < income_planets.size(); ++i) {
            // WGEP-05: a blocked collection returns before the refresh, so the retained source stays.
            if (contributions.value()[i].blocked) continue;
            const auto owner = std::lower_bound(fiscal.players.begin(), fiscal.players.end(), income_planets[i].owner,
                [](const auto& row, auto id) { return row.player < id; });
            if (owner == fiscal.players.end() || owner->player != income_planets[i].owner || !owner->earns_planet_income) continue;
            auto& source = impl.control->state[i].political_income;
            if (source == contributions.value()[i].political) continue;
            source = contributions.value()[i].political;
            impl.dirty[static_cast<std::size_t>(StateSection::control)] = true;
            ++impl.income_control_generation;
        }
        std::vector<math::Fixed> totals(fiscal.players.size());
        // Ordered owner reduction of the partitioned per-planet result.
        if (impl.economy_corruption) {
            std::vector<math::Fixed> values;
            for (const auto& row : contributions.value()) values.push_back(row.total);
            auto paid = corruption_payments(*impl.economy_corruption, income_planets, fiscal.players,
                values, tick_clock.galactic_frame, executor, true);
            if (!paid) { impl.failed = true; return core::Result<void>::failure(paid.error()); }
            totals = std::move(paid).value();
            impl.dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
        } else for (std::size_t i = 0; i < income_planets.size(); ++i) {
            const auto player = std::lower_bound(fiscal.players.begin(), fiscal.players.end(), income_planets[i].owner,
                [](const auto& row, PlayerId id) { return row.player < id; });
            if (player == fiscal.players.end() || player->player != income_planets[i].owner || !player->earns_planet_income) continue;
            const auto index = static_cast<std::size_t>(player - fiscal.players.begin());
            auto sum = math::add(totals[index], contributions.value()[i].total);
            if (!sum) {
                impl.failed = true;
                return fail<void>(diagnostic_codes::worker_failure, sum.error().message);
            }
            totals[index] = sum.value();
        }
        // WGEP-03/15/16: one nonzero aggregate per player, no maintenance/tax
        // debit. WGEP-112/WGA-13..15: commit the account first, then
        // deliver gross income independently of multiplier and cap.
        auto& ledger = *impl.economy;
        for (std::size_t i = 0; i < totals.size(); ++i) {
            // WGEP-112/WGA-14: zero aggregate skips account addition
            // and the AI callback, including its goal-time decrement.
            if (totals[i].raw() == 0) continue;
            auto result = adjusted_credits(ledger.players[i].credits, totals[i],
                fiscal.players[i].positive_credit_multiplier, ledger.players[i].credit_cap);
            if (!result) {
                impl.failed = true;
                return fail<void>(diagnostic_codes::worker_failure, "fiscal ledger: " + result.error().message);
            }
            if (ledger.players[i].credits != result.value()) {
                ledger.players[i].credits = result.value();
                impl.dirty[static_cast<std::size_t>(StateSection::players)] = true;
            }
            if (impl.credit_notification_handler) {
                const auto seconds = math::Fixed::from_ratio(duration, 30);
                if (!seconds) {
                    impl.failed = true; return core::Result<void>::failure(seconds.error());
                }
                auto notified = impl.credit_notification_handler({CreditNotification::Kind::fiscal,
                    ledger.players[i].player, result.value(), totals[i], seconds.value()});
                if (!notified) {
                    impl.failed = true; return core::Result<void>::failure(notified.error());
                }
            }
            // WGEP-16: each player's account callback precedes cycle increment.
            if (totals[i].raw() != 0) {
                if (auto v = impl.test_credit_condition(ledger.players[i].player, executing, events); !v) {
                    impl.failed = true; return core::Result<void>::failure(v.error());
                }
            }
        }
    }
    if (impl.economy_corruption) {
        auto rolled = reroll_planet_income(*impl.economy_corruption, impl.setup.seed);
        if (!rolled) { impl.failed = true; return core::Result<void>::failure(rolled.error()); }
        impl.income_forecast_cache.reset();
        impl.dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    }
    tick_clock.fiscal_start = tick_clock.fiscal_end;
    tick_clock.fiscal_end += duration;
    ++tick_clock.fiscal_cycle;
    ++tick.work.fiscal_cycles;
    events.push_back({executing, GalacticEventKind::fiscal_cycle_completed, {0}, invalid_entity_id, 0,
        tick_clock.fiscal_cycle, GalacticRejectReason::none});
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::Impl::service_tick_clock(const PartitionExecutor& executor, const std::uint64_t executing,
    std::vector<GalacticEvent>& events, GalacticTick& tick, const bool frame_admitted,
    const bool step_timer) {
    auto& impl = *this;
    auto& tick_clock = impl.clock;
    // Legacy clock-only streams keep their frame convention. Configured fiscal
    // reads current mode time (WGEP-59/GFD-04). The conflict-aware mode manager
    // advances the active timer at its final outer tail (GFD-26/GFSC-18).
    if (step_timer && !impl.setup.fiscal && !impl.victory) ++tick_clock.galactic_frame;
    // WGM-03, WGEP-01: one `if`, not a catch-up loop: at most one fiscal cycle per service. The
    // payment (WGEP-03..17) hooks in here with the fiscal ledger.
    const auto duration = impl.setup.rules.fiscal_cycle_frames;
    if (duration != 0 && tick_clock.galactic_frame >= tick_clock.fiscal_end) {
        if (auto result = impl.service_tick_fiscal(executor, executing, events, tick, duration); !result)
            return core::Result<void>::failure(result.error());
    }
    // WGM-04: after fiscal work, the periodic checks on every frame divisible by 30 (leader-death
    // victory and destroyed-planet notices hook in here with their tickets).
    if (frame_admitted && tick_clock.galactic_frame % periodic_check_frames == 0) ++tick.work.periodic_checks;
    if (impl.victory) {
        gc_profile::Scope victory_scope(gc_profile::Phase::tick_victory);
        // WGM-03/04: fiscal and leader callbacks are distinct admissions.
        if (tick.work.fiscal_cycles) {
            const std::array kinds{VictoryKind::cycles};
            if (auto v = impl.test_conditions(executor, kinds, executing, events); !v) {
                impl.failed = true; return core::Result<void>::failure(v.error());
            }
        }
        if (frame_admitted && tick_clock.galactic_frame % periodic_check_frames == 0) {
            // WGS-171: this producer requests only kill-enemy-leader.
            const std::array kinds{VictoryKind::kill_leader};
            if (auto v = impl.test_conditions(executor, kinds, executing, events); !v) {
                impl.failed = true; return core::Result<void>::failure(v.error());
            }
        }
        // GFS-15/GFD-04: countdown after fiscal, before active-mode timer advance.
        if (!impl.victory_tail_deferred) impl.service_victory_tail(executing, events);
    }
    if ((impl.setup.fiscal || impl.victory) && step_timer && !impl.victory_tail_deferred) ++tick_clock.galactic_frame;
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::Impl::commit_tick_commands(const PartitionExecutor& executor, const std::uint64_t executing,
    std::vector<GalacticEvent>& events, const bool pending_local_conflict) {
    auto& impl = *this;
    std::optional<gc_profile::Scope> commands_scope(std::in_place, gc_profile::Phase::tick_commands);
    while (!impl.pending.empty() && impl.pending.begin()->first.tick == executing) {
        auto command = std::move(impl.pending.begin()->second);
        impl.pending.erase(impl.pending.begin());
        // GFS-01: admission reads state left by earlier synchronized commands.
        if (auto refreshed = impl.refresh_population(executor); !refreshed) {
            impl.failed = true; return core::Result<void>::failure(refreshed.error());
        }
        impl.execute(command, executing, events, pending_local_conflict);
        if (impl.failed) return fail<void>(diagnostic_codes::resource_limit, "credit victory admission failed during command commit");
        impl.executed.push_back(std::move(command));
    }

    commands_scope.reset();
    return core::Result<void>::success();
}

void GalacticSession::set_prebattle_autosave_window(const bool open) noexcept { impl_->prebattle_autosave_window = open; }
void GalacticSession::set_system_hold(const bool held) noexcept { impl_->system_hold = held; }

core::Result<GalacticTick> GalacticSession::step(const PartitionExecutor& executor,
    const bool pending_local_conflict) {
    auto& impl = *impl_;
    if (impl.completed_tick >= galactic_max_ticks) {
        return fail<GalacticTick>(diagnostic_codes::simulation_complete, "the galaxy reached its tick limit");
    }
    if (impl.failed) {
        return fail<GalacticTick>(diagnostic_codes::worker_failure, "an earlier phase failed; the session cannot continue");
    }
    if (native_barrier()) {
        return fail<GalacticTick>(diagnostic_codes::invalid_command, "galactic service requires a completed native action barrier");
    }
    if (auto replayed = replay_native_barrier(executor); !replayed) {
        impl.failed = true; return core::Result<GalacticTick>::failure(replayed.error());
    }
    GalacticTick tick;
    const auto projection_records_before = impl.observer_projection_records;
    const auto income_sources_before = impl.income_sources_gathered;
    const auto income_modifiers_before = impl.income_modifier_rebuilds;
    const auto offer_rebuilds_before = impl.production_offer_rebuilds;
    impl.population_objects = 0; impl.population_producers = 0;
    impl.control_effects.clear();

    // Commands of this tick, canonical order, serial commit.
    std::vector<GalacticEvent> events;
    const auto executing = impl.completed_tick;
    auto committed = impl.commit_tick_commands(executor, executing, events, pending_local_conflict);
    impl.prebattle_autosave_window = false; // one step only: session controls during a child never see it
    if (!committed) return core::Result<GalacticTick>::failure(committed.error());
    // GFD-23 / GFSC-13: discovery reads prior committed presence before movement.
    // WGUI-01: paused passes reset orbit counters; allied/corruption reveal still qualifies.
    if (auto result = gc_profile::timed(gc_profile::Phase::tick_discovery, [&] { return impl.service_discovery(executor, executing, events); }); !result) {
        impl.failed = true; return core::Result<GalacticTick>::failure(result.error());
    }
    tick.work.discovery_records = impl.discovery_records;
    tick.work.discovery_targets = impl.discovery_targets;

    // A paused galaxy runs no service: no phase, no frame, no fiscal or periodic check (WGM-05/06).
    const bool frame_admitted = impl.clock.pause_depth == 0;
    const auto history_tick = impl.clock.galactic_frame;
    if (frame_admitted) {
        if (auto result = impl.service_tick_objects(*this, executor, executing, events, tick, pending_local_conflict); !result)
            return core::Result<GalacticTick>::failure(result.error());
    }
    // WGM-89: shared-object conflict tail follows movement/control and precedes fiscal.
    // It also progresses paused pending/auto-resolve states; no ordinary object service
    // is admitted by that progression (WGT-02, WGM-47).
    if (impl.conflict_tail) {
        gc_profile::Scope tail_scope(gc_profile::Phase::tick_conflict_tail);
        const auto forecast_before = impl.income_forecast_rebuilds;
        impl.snapshot = impl.make_snapshot(events, &executor);
        tick.work.income_forecast_rebuilds = impl.income_forecast_rebuilds - forecast_before;
        tick.work.income_sources_gathered = impl.income_sources_gathered - income_sources_before;
        tick.work.income_modifier_rebuilds = impl.income_modifier_rebuilds - income_modifiers_before;
        auto serviced = impl.conflict_tail(*this);
        if (!serviced) { impl.failed = true; return core::Result<GalacticTick>::failure(serviced.error()); }
        events.assign(impl.snapshot->events().begin(), impl.snapshot->events().end());
    }
    // GFS-A18/GFD-A13/WGA-379: object service, including the conflict tail,
    // completes before timeline reduction and later fiscal/AI readers. The
    // separate presentation/collision/movement-support phases remain seams.
    // Mode pause does not gate an admitted shared-mode invocation.
    if (impl.timeline_service) {
        auto updated = impl.timeline_service(impl.economy);
        if (!updated) { impl.failed = true; return core::Result<GalacticTick>::failure(updated.error()); }
    }
    // A tail without the outer timer keeps the tail-free frame convention (admitted frames only).
    const bool step_timer = frame_admitted && !(impl.conflict_tail && impl.conflict_tail_outer_timer);
    if (frame_admitted || impl.conflict_tail) {
        if (auto result = impl.service_tick_clock(executor, executing, events, tick, frame_admitted, step_timer); !result)
            return core::Result<GalacticTick>::failure(result.error());
    }

    if (frame_admitted && !impl.history_service_deferred) {
        if (auto recorded = service_admitted_history(history_tick, executor); !recorded) {
            impl.failed = true; return core::Result<GalacticTick>::failure(recorded.error());
        }
    }
    impl.expire_technology_prices();
    impl.expire_income_blockers();
    ++impl.completed_tick;
    // GFS-09/GFC-08: publish after actual queue service, not on a fiscal cadence.
    if (impl.dirty[static_cast<std::size_t>(StateSection::planets)]
        || impl.dirty[static_cast<std::size_t>(StateSection::fleets)]
        || impl.dirty[static_cast<std::size_t>(StateSection::production)]) impl.population_stale = true;
    if (auto refreshed = gc_profile::timed(gc_profile::Phase::tick_population, [&] { return impl.refresh_population(executor); }); !refreshed) {
        impl.failed = true; return core::Result<GalacticTick>::failure(refreshed.error());
    }
    tick.work.population_objects = impl.population_objects;
    tick.work.population_producers = impl.population_producers;
    impl.dirty[static_cast<std::size_t>(StateSection::session)] = true;

    tick.state_hash = gc_profile::timed(gc_profile::Phase::tick_state_hash, [&] { return impl.refresh_hash(&tick.work); });
    if (!impl.hasher) tick.state_sha256 = tick.state_hash.get();
    tick.completed_tick = impl.completed_tick;
    const auto forecast_before = impl.income_forecast_rebuilds;
    impl.snapshot = impl.make_snapshot(std::move(events), &executor);
    if (auto delivered = gc_profile::timed(gc_profile::Phase::tick_actor_delivery, [&] { return impl.deliver_actor_changes(); }); !delivered)
        return core::Result<GalacticTick>::failure(delivered.error());
    // Project reconstruction: external native admissions see the completed
    // ordinary publication. Their census invalidation belongs to the next
    // refresh, exactly as it does when admitted after a live step returns.
    if (impl.native_replaying) {
        if (auto replayed = replay_native_barrier(executor); !replayed) {
            impl.failed = true; return core::Result<GalacticTick>::failure(replayed.error());
        }
        tick.state_hash = impl.refresh_hash(&tick.work);
        if (!impl.hasher) tick.state_sha256 = tick.state_hash.get();
    }
    tick.work.observer_projection_records = impl.observer_projection_records - projection_records_before;
    tick.work.production_offer_rebuilds = impl.production_offer_rebuilds - offer_rebuilds_before;
    tick.work.income_forecast_rebuilds = impl.income_forecast_rebuilds - forecast_before;
    tick.work.income_sources_gathered = impl.income_sources_gathered - income_sources_before;
    tick.work.income_modifier_rebuilds = impl.income_modifier_rebuilds - income_modifiers_before;
    tick.snapshot = impl.snapshot;
    return core::Result<GalacticTick>::success(std::move(tick));
}

core::Result<void> GalacticSession::apply_session_controls(const std::span<const GameCommand> commands,
    const bool pending_local_conflict, const bool append_events,
    const std::optional<std::pair<std::uint64_t, std::uint32_t>> observer_load_clock) {
    if (observer_load_clock && observer_load_clock->second == 0)
        return fail<void>(diagnostic_codes::invalid_command, "observer load clock needs a logical frame rate");
    // Validate the whole batch before executing any control.
    for (const auto& command : commands) {
        const auto opcode = opcode_of(command.payload);
        if (impl_->seat(command.key.seat) == nullptr || layer_of(command.payload) != CommandLayer::session
            || opcode == session_opcodes::debug_set_planet_owner || opcode == session_opcodes::debug_set_fleet_orbit
            || decode_payload(CommandLayer::session, opcode, encode_payload(command.payload)) != std::optional(command.payload)) {
            return fail<void>(diagnostic_codes::invalid_command, "battle service accepts only valid session controls");
        }
    }
    std::vector<GalacticEvent> events;
    if (append_events && impl_->snapshot) events.assign(impl_->snapshot->events().begin(), impl_->snapshot->events().end());
    for (const auto& command : commands) {
        if (std::holds_alternative<PostLoadFixup>(command.payload)) {
            const auto [frame, fps] = observer_load_clock.value_or(std::pair{impl_->clock.galactic_frame,
                impl_->observer ? impl_->observer->logical_fps : galactic_frames_per_second});
            impl_->refresh_observers_for_load(frame, fps);
        }
        impl_->execute(command, command.key.tick, events, pending_local_conflict);
    }
    impl_->snapshot = impl_->make_snapshot(std::move(events));
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::reconcile_battle(const EntityId first_free,
    const std::uint64_t session_tick, const std::uint64_t tactical_frame) {
    if (first_free == invalid_entity_id || impl_->next_id == invalid_entity_id || first_free < impl_->next_id) {
        return fail<void>(diagnostic_codes::invalid_setup, "battle return cannot rewind the shared allocator");
    }
    if (session_tick < impl_->completed_tick || session_tick > galactic_max_ticks || !impl_->pending.empty()) {
        return fail<void>(diagnostic_codes::invalid_setup, "battle return requires an ordered session frontier and no galaxy-owned pending input");
    }
    impl_->next_id = first_free;
    // WGT-01: no galaxy service happened during the child, but the shared session frontier
    // advanced. Seal it at reconciliation so late-command validation and the no-child hash
    // still describe the whole game, including the elapsed tactical frames (WGS-42).
    impl_->completed_tick = session_tick;
    impl_->clock.session_tick = session_tick;
    impl_->clock.tactical_frame = tactical_frame;
    impl_->dirty[static_cast<std::size_t>(StateSection::session)] = true;
    const auto previous = impl_->snapshot->events();
    std::vector<GalacticEvent> events;
    // Only controls issued on this return step belong to its publication.
    // Earlier galaxy events can remain in the frozen snapshot throughout a child.
    for (const auto& event : previous)
        if (session_tick == 0 || event.tick >= session_tick - 1) events.push_back(event);
    impl_->snapshot = impl_->make_snapshot(std::move(events));
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::apply_battle_losses(const std::span<const EntityId> destroyed) {
    return apply_battle_losses(destroyed, {});
}

const CampaignScoreLedger& GalacticSession::score_ledger() const noexcept { return impl_->score_ledger; }

core::Result<void> GalacticSession::record_player_quit(const PlayerId player) {
    const auto& players = impl_->setup.players;
    if (impl_->failed || std::none_of(players.begin(), players.end(),
        [&](const GalacticPlayer& row) { return row.player_id == player; }))
        return fail<void>(diagnostic_codes::invalid_setup, "player quit names no listed player");
    if (impl_->score_ledger.quits.insert(player).second) impl_->touch_score_ledger();
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::apply_battle_losses(const std::span<const EntityId> destroyed,
    const std::span<const BattleKiller> killers) {
    if (impl_->failed || destroyed.size() > galactic_max_objects
        || !std::is_sorted(destroyed.begin(), destroyed.end())
        || std::adjacent_find(destroyed.begin(), destroyed.end()) != destroyed.end()
        || !std::is_sorted(killers.begin(), killers.end(), [](const auto& a, const auto& b) { return a.id < b.id; })
        || std::any_of(destroyed.begin(), destroyed.end(), [&](const auto id) {
            return id == 0 || id >= impl_->next_id || impl_->planets.contains(id) || impl_->routes.contains(id);
        })) return fail<void>(diagnostic_codes::invalid_setup, "invalid ordered battle casualty identities");
    if (destroyed.empty()) return core::Result<void>::success();
    const auto dead = [&](const EntityId id) { return std::binary_search(destroyed.begin(), destroyed.end(), id); };
    // WGP-98: the victim's owner and original type are read before the removal below. One
    // bounded pass per source; a loss without a represented type is not a scoring death.
    {
        std::map<EntityId, std::pair<PlayerId, TypeId>> victims;
        for (const auto& [fleet_id, handle] : impl_->fleets) {
            (void)fleet_id;
            const auto& fleet = impl_->registry.get<FleetData>(handle);
            for (const auto& member : fleet.members) if (dead(member.id)) victims.emplace(member.id, std::pair{fleet.owner, member.type});
        }
        for (const auto& [id, unit] : impl_->detached_units) if (dead(id)) victims.emplace(id, std::pair{unit.owner, unit.member.type});
        if (impl_->production) for (const auto& planet : impl_->production->planets)
            for (const auto& object : planet.objects) if (dead(object.id)) victims.emplace(object.id, std::pair{object.owner, object.type});
        for (const auto id : destroyed) {
            std::pair<PlayerId, TypeId> victim{};
            if (const auto record = impl_->actor_records.find(id); record != impl_->actor_records.end()) {
                victim = {record->second.owner, record->second.original_type};
            } else if (const auto found = victims.find(id); found != victims.end()) victim = found->second;
            if (!victim.second) continue;
            const auto blow = std::lower_bound(killers.begin(), killers.end(), id,
                [](const BattleKiller& row, const EntityId key) { return row.id < key; });
            impl_->record_death(victim.first, blow != killers.end() && blow->id == id ? blow->killer : PlayerId{}, victim.second);
        }
    }
    // WAR-45: this is a bounded conflict-return commit, not per-frame entity work.
    std::set<EntityId> removed_fleets;
    for (auto it = impl_->fleets.begin(); it != impl_->fleets.end();) {
        auto& fleet = impl_->registry.get<FleetData>(it->second);
        std::erase_if(fleet.members, [&](const auto& member) { return dead(member.id); });
        if (dead(it->first) || fleet.members.empty()) {
            removed_fleets.insert(it->first);
            impl_->registry.destroy(it->second); it = impl_->fleets.erase(it); --impl_->live_object_count;
        } else {
            if (fleet.motion.transit) std::erase_if(fleet.motion.transit->packed, dead);
            ++it;
        }
    }
    std::erase_if(impl_->detached_units, [&](const auto& row) { return dead(row.first); });
    for (auto id : destroyed) impl_->lose_actor(id);
    for (auto id : removed_fleets) impl_->lose_actor(id);
    if (impl_->movement_inputs) {
        auto movement = std::make_shared<MovementSetup>(*impl_->movement_inputs);
        std::erase_if(movement->contents, [&](auto& row) {
            std::erase_if(row.children, [&](const auto& member) { return dead(member.id); });
            return dead(row.container);
        });
        impl_->movement_inputs = std::move(movement);
    }
    // WGT-115: persistent orbital sources share the ordinary battle-loss commit.
    if (impl_->production) {
        bool removed{};
        for (std::size_t index = 0; index < impl_->production->planets.size(); ++index) {
            auto& planet = impl_->production->planets[index];
            const bool ground_special = std::any_of(planet.objects.begin(), planet.objects.end(), [&](const auto& object) {
                return dead(object.id) && object.domain == ProductionDomain::ground && object.kind == ProductionKind::structure;
            });
            const auto count = std::erase_if(planet.objects, [&](const auto& object) { return dead(object.id); });
            impl_->live_object_count -= count; removed |= count != 0;
            if (ground_special) impl_->refresh_ground_base_level(index); // WGM-188
        }
        if (removed) impl_->dirty[static_cast<std::size_t>(StateSection::production)] = true;
    }
    if (std::erase_if(impl_->production_originals, [&](const auto& row) { return dead(row.id) || removed_fleets.contains(row.id); }))
        impl_->published_production_originals.reset();
    impl_->population_stale = true;
    if (impl_->setup.census) impl_->dirty[static_cast<std::size_t>(StateSection::census)] = true;
    impl_->dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
    impl_->invalidate_observer_projection();
    if (impl_->setup.fleet_control) impl_->dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
    if (impl_->setup.movement) impl_->dirty[static_cast<std::size_t>(StateSection::movement)] = true;
    const auto events = impl_->snapshot->events();
    impl_->snapshot = impl_->make_snapshot(std::vector<GalacticEvent>(events.begin(), events.end()));
    return impl_->deliver_actor_changes();
}

std::span<const OutcomeJournalEntry> GalacticSession::pending_outcomes() const noexcept {
    return std::span(impl_->setup.outcomes).subspan(impl_->applied_outcomes);
}
void GalacticSession::append_outcome_events(const PlanetId planet, const EntityId conflict, const PlayerId winner, const PlayerId loser) {
    std::vector<GalacticEvent> events(impl_->snapshot->events().begin(), impl_->snapshot->events().end());
    events.push_back({impl_->completed_tick, GalacticEventKind::auto_resolve_won, {}, planet, 0, winner, GalacticRejectReason::none});
    events.push_back({impl_->completed_tick, GalacticEventKind::auto_resolve_lost, {}, planet, 0, loser, GalacticRejectReason::none});
    events.push_back({impl_->completed_tick, GalacticEventKind::auto_resolve_completed, {}, conflict, 0, winner, GalacticRejectReason::none});
    impl_->snapshot = impl_->make_snapshot(std::move(events));
}
core::Result<void> GalacticSession::stage_outcome(OutcomeJournalEntry entry) {
    if (impl_->failed || entry.completed_tick <= impl_->completed_tick || entry.completed_tick > galactic_max_ticks
        || !entry.conflict || entry.payload.empty() || entry.payload.size() > 1'048'576
        || impl_->setup.outcomes.size() >= galactic_max_objects
        || (!impl_->setup.outcomes.empty() && entry.completed_tick < impl_->setup.outcomes.back().completed_tick)
        || std::any_of(impl_->setup.outcomes.begin(), impl_->setup.outcomes.end(),
            [&](const auto& row) { return row.conflict == entry.conflict; }))
        return fail<void>(diagnostic_codes::invalid_setup, "invalid trusted outcome journal entry");
    impl_->setup.outcomes.push_back(std::move(entry));
    return core::Result<void>::success();
}
core::Result<void> GalacticSession::commit_outcome(const OutcomeJournalEntry& entry, const OutcomeCommit& apply,
    const PartitionExecutor& executor) {
    const auto pending = pending_outcomes();
    if (impl_->failed || pending.empty() || pending.front() != entry || entry.completed_tick != impl_->completed_tick || !apply)
        return fail<void>(diagnostic_codes::invalid_setup, "outcome requires its recorded campaign return boundary");
    if (auto committed = apply(entry); !committed) {
        // As with a failed service phase, a failed effect delivery seals the session.
        impl_->failed = true; return committed;
    }
    // This selected return can change ownership, fleet membership and production
    // after the ordinary galaxy service. Seal its derived census before publication.
    if (auto refreshed = refresh_movement_access(executor); !refreshed) return refreshed;
    if (impl_->setup.census) {
        impl_->population_stale = true;
        if (auto refreshed = impl_->refresh_population(executor); !refreshed) {
            impl_->failed = true; return refreshed;
        }
    }
    const auto events = impl_->snapshot->events();
    impl_->snapshot = impl_->make_snapshot(std::vector<GalacticEvent>(events.begin(), events.end()));
    ++impl_->applied_outcomes;
    impl_->dirty[static_cast<std::size_t>(StateSection::outcomes)] = true;
    return core::Result<void>::success();
}

void GalacticSession::set_state_hasher(std::shared_ptr<StateHasher> hasher) noexcept { impl_->hasher = std::move(hasher); }
const EconomyCorruptionState* GalacticSession::economy_corruption() const noexcept {
    return impl_->economy_corruption ? &*impl_->economy_corruption : nullptr;
}

void GalacticSession::enable_conflict_requests(bool enabled) { impl_->conflict_requests_enabled = enabled; }
void GalacticSession::set_conflict_tail_handler(ConflictTailHandler handler, const bool outer_timer) {
    impl_->conflict_tail = std::move(handler);
    impl_->conflict_tail_outer_timer = outer_timer;
}

void GalacticSession::set_credit_notification_handler(CreditNotificationHandler handler) {
    impl_->credit_notification_handler = std::move(handler);
}
void GalacticSession::set_feedback_notice_handler(FeedbackNoticeHandler handler) {
    impl_->feedback_notice_handler = std::move(handler);
}

void GalacticSession::set_native_production_outcome_handler(NativeProductionOutcomeHandler handler) {
    impl_->native_production_outcome_handler = std::move(handler);
}
void GalacticSession::set_native_actor_changes_handler(NativeActorChangesHandler handler) {
    impl_->native_actor_changes_handler = std::move(handler);
}

core::Result<math::Fixed> GalacticSession::add_credits(PlayerId player, math::Fixed delta,
    math::Fixed positive_multiplier) {
    if (impl_->failed || !impl_->economy)
        return fail<math::Fixed>(diagnostic_codes::invalid_setup, "credit addition requires a live account");
    auto& accounts = impl_->economy->players;
    const auto at = std::lower_bound(accounts.begin(), accounts.end(), player,
        [](const auto& row, PlayerId id) { return row.player < id; });
    if (at == accounts.end() || at->player != player)
        return fail<math::Fixed>(diagnostic_codes::invalid_setup, "credit addition names a missing player");
    if (auto* shadow = native_shadow()) return stage_native_credits(*shadow, player, delta, positive_multiplier);
    auto balance = adjusted_credits(at->credits, delta, positive_multiplier, at->credit_cap);
    if (!balance) return balance;
    auto actual = math::subtract(balance.value(), at->credits);
    if (!actual) return actual;
    // Validate the retained ledger transaction before committing the account.
    if (impl_->credit_notification_handler) {
        auto notified = impl_->credit_notification_handler({CreditNotification::Kind::windfall,
            player, balance.value(), actual.value(), {}});
        if (!notified) return core::Result<math::Fixed>::failure(notified.error());
    }
    at->credits = balance.value();
    impl_->dirty[static_cast<std::size_t>(StateSection::players)] = true;
    const auto events = impl_->snapshot->events();
    std::vector<GalacticEvent> published(events.begin(), events.end());
    auto victory = impl_->test_credit_condition(player, impl_->completed_tick, published);
    if (!victory) {
        impl_->failed = true; return core::Result<math::Fixed>::failure(victory.error());
    }
    impl_->snapshot = impl_->publish_native_snapshot(std::move(published));
    return actual;
}
void GalacticSession::finish_conflict_frame_timer() {
    if (impl_->clock.pause_depth) return;
    ++impl_->clock.galactic_frame;
    impl_->expire_technology_prices();
    impl_->expire_income_blockers();
    impl_->dirty[static_cast<std::size_t>(StateSection::session)] = true;
    std::vector<GalacticEvent> events(impl_->snapshot->events().begin(), impl_->snapshot->events().end());
    impl_->snapshot = impl_->make_snapshot(std::move(events));
}
core::Result<void> GalacticSession::adjust_conflict_pause(bool begin) {
    // WGM-05/06, WGT-02: the conflict service owns one nested pause, independent of
    // player pauses. A player may already have consumed that level with ResumeGalaxy.
    if (begin && impl_->clock.pause_depth == std::numeric_limits<std::uint32_t>::max())
        return fail<void>(diagnostic_codes::resource_limit, "conflict pause depth overflow");
    if (begin) ++impl_->clock.pause_depth;
    else if (impl_->clock.pause_depth) --impl_->clock.pause_depth;
    impl_->dirty[static_cast<std::size_t>(StateSection::session)] = true;
    std::vector<GalacticEvent> events(impl_->snapshot->events().begin(), impl_->snapshot->events().end());
    impl_->snapshot = impl_->make_snapshot(std::move(events));
    return core::Result<void>::success();
}
core::Result<void> GalacticSession::reserve_conflict_entities(EntityId first_free) {
    if (!first_free || !impl_->next_id || first_free < impl_->next_id)
        return fail<void>(diagnostic_codes::invalid_setup, "conflict reservation rewinds the shared allocator");
    impl_->next_id = first_free;
    impl_->dirty[static_cast<std::size_t>(StateSection::session)] = true;
    std::vector<GalacticEvent> events(impl_->snapshot->events().begin(), impl_->snapshot->events().end());
    impl_->snapshot = impl_->make_snapshot(std::move(events));
    return core::Result<void>::success();
}
void GalacticSession::publish_conflict_events(std::span<const GalacticEvent> incoming) {
    if (incoming.empty()) return;
    std::vector<GalacticEvent> events(impl_->snapshot->events().begin(), impl_->snapshot->events().end());
    events.insert(events.end(), incoming.begin(), incoming.end());
    impl_->snapshot = impl_->make_snapshot(std::move(events));
}

std::uint64_t GalacticSession::completed_tick() const noexcept { return impl_->completed_tick; }
GalacticClock GalacticSession::clock() const noexcept {
    auto result = impl_->clock;
    result.session_tick = impl_->completed_tick;
    return result;
}
EntityId GalacticSession::next_entity_id() const noexcept { if (const auto* shadow = native_shadow()) return shadow->next_entity_id(); return impl_->next_id; }
const std::optional<VictoryState>& GalacticSession::victory() const noexcept { return impl_->victory; }
bool GalacticSession::multiplayer() const noexcept { return impl_->multiplayer_flag(); }
const GalacticSetup& GalacticSession::setup() const noexcept { return impl_->setup; }
std::span<const Seat> GalacticSession::seats() const noexcept { return impl_->seats; }
const std::optional<SeatControlState>& GalacticSession::seat_control() const noexcept { return impl_->seat_control; }
core::Result<void> GalacticSession::begin_battle_roles(const EntityId conflict, const PlayerId invader, const PlayerId defender,
    const bool default_spectate) {
    if (!impl_->seat_control) return core::Result<void>::success();
    if (impl_->seat_control->battle || conflict == 0 || invader == defender)
        return fail<void>(diagnostic_codes::invalid_setup, "battle role context is already active or invalid");
    const auto human_side = [&](const PlayerId player) {
        const auto found = std::find_if(impl_->seats.begin(), impl_->seats.end(), [&](const auto& seat) {
            return seat.kind == SeatKind::human && seat.active && seat.player == player;
        });
        return found == impl_->seats.end() ? HumanSideId{} : found->human_side_id;
    };
    const auto invader_side = human_side(invader), defender_side = human_side(defender);
    if ((invader_side != 0) == (defender_side != 0)) return core::Result<void>::success();
    const auto fighting_side = invader_side != 0 ? invader_side : defender_side;
    const auto& teams = impl_->seat_control->control.teams;
    const auto other = std::find_if(teams.begin(), teams.end(), [&](const auto& team) { return team.side != fighting_side && team.lead.value != 0; });
    if (other == teams.end()) return core::Result<void>::success();
    const auto human = invader_side != 0 ? invader : defender;
    const auto ai = invader_side != 0 ? defender : invader;
    impl_->seat_control->battle = BattleRoleChoice{conflict, human, ai, other->side, other->observer_ai ? ai : human,
        default_spectate ? BattleTeamRole::spectate : BattleTeamRole::pending};
    impl_->dirty[static_cast<std::size_t>(StateSection::session)] = true;
    std::vector<GalacticEvent> events;
    if (impl_->snapshot) events.assign(impl_->snapshot->events().begin(), impl_->snapshot->events().end());
    impl_->snapshot = impl_->make_snapshot(std::move(events));
    return core::Result<void>::success();
}
void GalacticSession::end_battle_roles() {
    if (!impl_->seat_control || !impl_->seat_control->battle) return;
    impl_->seat_control->battle.reset();
    impl_->dirty[static_cast<std::size_t>(StateSection::session)] = true;
    const auto events = impl_->snapshot->events();
    impl_->snapshot = impl_->make_snapshot(std::vector<GalacticEvent>(events.begin(), events.end()));
}
const PlanetGraph* GalacticSession::graph() const noexcept { return impl_->graph ? &*impl_->graph : nullptr; }
std::size_t GalacticSession::pending_command_count() const noexcept { return impl_->pending.size(); }
std::span<const GameCommand> GalacticSession::executed_commands() const noexcept { return impl_->executed; }

GalacticReplay GalacticSession::record() const {
    // The replay format numbers its journal from 1; after a history rebase the window follows the dropped entries (#4058).
    GalacticReplay replay{impl_->setup, impl_->completed_tick, impl_->executed, impl_->native_effects};
    for (auto& effect : replay.native_effects) effect.sequence -= impl_->native_dropped;
    return replay;
}
std::shared_ptr<const GalacticSnapshot> GalacticSession::snapshot() const noexcept { if (const auto* shadow = native_shadow()) return shadow->snapshot(); return impl_->snapshot; }
std::shared_ptr<const GalacticSnapshot> GalacticSession::view_snapshot(ObserverViewContext view) const {
    if (!view.player || !impl_->observer || !std::binary_search(impl_->observer->human_observers.begin(), impl_->observer->human_observers.end(), view.player)) return {};
    if (view.player == impl_->observer->solo_reference_human) {
        // The completed solo snapshot already publishes this reference human's
        // offers, forecast and services. Share those immutable columns; only its
        // all-player observation rows need the explicit private-view filter.
        auto& cache = impl_->view_publications[view.player];
        if (!cache.observer_projection || cache.observer_projection_generation != impl_->snapshot->observer_input_generation()) {
            auto observations = std::make_shared<std::vector<ObservedPlanet>>();
            observations->reserve(impl_->observer->planets.size());
            for (const auto& row : impl_->snapshot->observations())
                if (row.player == view.player) observations->push_back(row);
            cache.observer_projection = std::move(observations);
            cache.observer_projection_generation = impl_->snapshot->observer_input_generation();
        }
        NativeSnapshotChanges projection;
        projection.observations = cache.observer_projection;
        return GalacticSnapshot::native_view(impl_->snapshot, std::move(projection));
    }
    return impl_->make_snapshot(std::vector<GalacticEvent>(impl_->snapshot->events().begin(), impl_->snapshot->events().end()), nullptr, view);
}

core::Result<bool> GalacticSession::set_planet_access(PlanetId planet, bool restricted,
    std::optional<bool> local_revealed) {
    if (!impl_->planet_access) return fail<bool>(diagnostic_codes::invalid_setup, "planet access owner is absent");
    const auto& rows = impl_->planet_access->planets;
    const auto at = std::lower_bound(rows.begin(), rows.end(), planet, [](const auto& row, auto id) { return row.planet < id; });
    if (at == rows.end() || at->planet != planet) return fail<bool>(diagnostic_codes::invalid_setup, "unknown planet access identity");
    const PlanetAccess value{planet, restricted, local_revealed};
    if (*at == value) return core::Result<bool>::success(false);
    if (impl_->planet_access->generation == std::numeric_limits<std::uint64_t>::max())
        return fail<bool>(diagnostic_codes::invalid_setup, "planet access generation exhausted");
    auto next = std::make_shared<PlanetAccessState>(*impl_->planet_access);
    next->planets[static_cast<std::size_t>(at - rows.begin())] = value; ++next->generation;
    impl_->planet_access = std::move(next);
    impl_->dirty[static_cast<std::size_t>(StateSection::planet_access)] = true;
    impl_->invalidate_observer_projection();
    const auto events = impl_->snapshot->events();
    impl_->snapshot = impl_->make_snapshot(std::vector<GalacticEvent>(events.begin(), events.end()));
    return core::Result<bool>::success(true);
}
core::Result<void> GalacticSession::restore_planet_access(std::span<const std::uint8_t> bytes) {
    auto restored = detail::decode_planet_access(bytes);
    if (!restored || !impl_->planet_access || restored->planets.size() != impl_->planets.size())
        return fail<void>(diagnostic_codes::invalid_setup, "invalid planet access state");
    std::size_t index{};
    for (const auto& [id, handle] : impl_->planets) {
        static_cast<void>(handle);
        if (restored->planets[index++].planet != id)
            return fail<void>(diagnostic_codes::invalid_setup, "planet access identity table differs");
    }
    impl_->planet_access = std::make_shared<const PlanetAccessState>(std::move(*restored));
    impl_->dirty[static_cast<std::size_t>(StateSection::planet_access)] = true;
    impl_->invalidate_observer_projection();
    const auto events = impl_->snapshot->events();
    impl_->snapshot = impl_->make_snapshot(std::vector<GalacticEvent>(events.begin(), events.end()));
    return core::Result<void>::success();
}

void GalacticSession::set_timeline_service(TimelineService service) { impl_->timeline_service = std::move(service); }
void GalacticSession::scramble_storage_for_testing() { impl_->rebuild(true); }

} // namespace eawr::sim::galactic
