#include "eawr/sim/galactic/session.hpp"
#include "eawr/core/gc_profile.hpp"

#include "eawr/core/sha256.hpp"

#include "encoding.hpp"
#include "../replay_internal.hpp"
#include "session_impl.hpp"
#include "checkpoint_fields_internal.hpp"
#include "checkpoint_commands_internal.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "eawr/sim/galactic/economy_corruption.hpp"
#include "eawr/sim/galactic/ability_host.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <limits>
#include <string>
#include <stdexcept>
#include <utility>

namespace eawr::sim::galactic {

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
        v.economy, v.production, v.income, v.economy_corruption, v.ability_host, v.score_ledger,
        v.victory, v.control, v.control_effects, v.observer, v.planet_access, v.movement_inputs, v.detached_units,
        v.production_originals, v.actor_records, v.pending_actor_changes, v.population,
        v.population_stale, v.live_object_count, v.applied_outcomes,
        v.pending, v.last_submitted, v.executed,
        v.next_native_invasion, v.native_invasions, v.native_invasion_queue,
        v.native_replay, v.native_replay_cursor, v.native_sequence,
        v.native_replay_end, v.native_replaying,
        v.corruption_due, v.active_producers, v.original_owned_counts, v.owned_planet_counts,
        v.income_blocker_next_expiry);
}

// Below this many objects the gather's small copies cost less on one partition than spread over
// the pool (the tactical gather's measured cutoff, #892). Scheduling only: never a result.
constexpr std::size_t gather_single_partition_below = 256;
bool hash_verification_requested() {
#ifdef _MSC_VER
    char* value{}; std::size_t size{};
    if (_dupenv_s(&value, &size, "EAWR_VERIFY_GALACTIC_HASH") || !value) return false;
    const bool enabled = std::string_view(value) == "1";
    std::free(value); return enabled;
#else
    const auto* value = std::getenv("EAWR_VERIFY_GALACTIC_HASH");
    return value && std::string_view(value) == "1";
#endif
}

[[nodiscard]] std::string command_context(const GameCommand& command) {
    return "command (tick " + std::to_string(command.key.tick) + ", seat " + std::to_string(command.key.seat.value)
        + ", sequence " + std::to_string(command.key.sequence) + ")";
}

template <typename T>
[[nodiscard]] core::Result<T> fail(const std::string_view code, std::string message) {
    return core::Result<T>::failure(detail::diagnostic(code, std::move(message)));
}

// SHA-256 over the encoding name and each section's ID, version and digest, in section order.
[[nodiscard]] std::string combine(const std::array<std::string, state_section_count>& section_hex, const std::vector<std::size_t>& ids) {
    std::vector<std::uint8_t> table(state_encoding_name.begin(), state_encoding_name.end());
    detail::put_u32(table, static_cast<std::uint32_t>(ids.size()));
    for (const auto section : ids) {
        detail::put_u16(table, static_cast<std::uint16_t>(section));
        detail::put_u16(table, detail::section_version);
        detail::put_bytes(table, detail::digest_bytes(section_hex[section]));
    }
    return core::sha256_hex(table);
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
        std::uint64_t journal_entries{}; in(journal_entries);
        if (journal_entries != journal.size() || impl_->native_sequence != journal.size() + 1U
            || (!journal.empty() && journal.back().frontier > impl_->completed_tick))
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
        for (const auto& row : fleets) { identity(row.id); impl_->insert(row); }
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
        impl_->published_economy_input_generation.reset();
        impl_->published_black_market_menus.reset(); impl_->published_ability_results.reset();
        impl_->published_corruption_transitions.reset();
        impl_->published_hero_action_menus.reset(); impl_->published_corruption_menu.reset();
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

void GalacticSession::Impl::insert(const Planet& planet) {
    const auto handle = registry.create();
    registry.emplace<StableId>(handle, planet.id);
    registry.emplace<PlanetData>(handle, planet.position, planet.owner);
    planets.emplace(planet.id, handle);
}

void GalacticSession::Impl::insert(const Route& route) {
    const auto handle = registry.create();
    registry.emplace<StableId>(handle, route.id);
    registry.emplace<RouteData>(handle, route.from, route.to, route.hs_speed_factor);
    routes.emplace(route.id, handle);
}

void GalacticSession::Impl::insert(const Fleet& fleet) {
    auto motion = fleet.motion;
    if (setup.movement && motion.orbit_entry == 0) {
        std::uint64_t latest = 0;
        for (const auto& [id, existing] : fleets) {
            (void)id; latest = std::max(latest, registry.get<FleetData>(existing).motion.orbit_entry);
        }
        motion.orbit_entry = latest + 1;
    }
    const auto handle = registry.create();
    registry.emplace<StableId>(handle, fleet.id);
    registry.emplace<FleetData>(handle, fleet.owner, fleet.orbit, fleet.members, fleet.orbit_slot, motion);
    fleets.emplace(fleet.id, handle);
}

void GalacticSession::Impl::rebuild(const bool reverse) {
    auto planet_rows = sorted_planets();
    auto route_rows = sorted_routes();
    auto fleet_rows = sorted_fleets();
    registry.clear();
    planets.clear();
    routes.clear();
    fleets.clear();
    if (reverse) {
        std::reverse(planet_rows.begin(), planet_rows.end());
        std::reverse(route_rows.begin(), route_rows.end());
        std::reverse(fleet_rows.begin(), fleet_rows.end());
        // Interleave the kinds the other way round too, so no storage keeps its old order.
        for (const auto& fleet : fleet_rows) insert(fleet);
        for (const auto& route : route_rows) insert(route);
        for (const auto& planet : planet_rows) insert(planet);
        return;
    }
    for (const auto& planet : planet_rows) insert(planet);
    for (const auto& route : route_rows) insert(route);
    for (const auto& fleet : fleet_rows) insert(fleet);
}

std::vector<Planet> GalacticSession::Impl::sorted_planets() const {
    std::vector<Planet> result;
    result.reserve(planets.size());
    for (const auto& [id, handle] : planets) {
        const auto& data = registry.get<PlanetData>(handle);
        result.push_back({id, data.position, data.owner});
    }
    return result;
}

std::vector<Route> GalacticSession::Impl::sorted_routes() const {
    std::vector<Route> result;
    result.reserve(routes.size());
    for (const auto& [id, handle] : routes) {
        const auto& data = registry.get<RouteData>(handle);
        result.push_back({id, data.from, data.to, data.hs_speed_factor});
    }
    return result;
}

std::vector<Fleet> GalacticSession::Impl::sorted_fleets() const {
    std::vector<Fleet> result;
    result.reserve(fleets.size());
    for (const auto& [id, handle] : fleets) {
        const auto& data = registry.get<FleetData>(handle);
        result.push_back({id, data.owner, data.orbit, data.members, data.orbit_slot, data.motion});
    }
    return result;
}

std::vector<std::pair<EntityId, ObjectKind>> GalacticSession::Impl::sorted_objects() const {
    std::vector<std::pair<EntityId, ObjectKind>> result;
    result.reserve(planets.size() + routes.size() + fleets.size());
    for (const auto& entry : planets) result.emplace_back(entry.first, ObjectKind::planet);
    for (const auto& entry : routes) result.emplace_back(entry.first, ObjectKind::route);
    for (const auto& entry : fleets) result.emplace_back(entry.first, ObjectKind::fleet);
    std::sort(result.begin(), result.end());
    return result;
}

GatheredObject GalacticSession::Impl::gather(const EntityId id, const ObjectKind kind) const {
    GatheredObject result{id, kind, {}, {}, {}};
    switch (kind) {
    case ObjectKind::planet: result.planet = registry.get<PlanetData>(planets.at(id)); break;
    case ObjectKind::route: result.route = registry.get<RouteData>(routes.at(id)); break;
    case ObjectKind::fleet: result.fleet = registry.get<FleetData>(fleets.at(id)); break;
    }
    return result;
}

std::vector<std::uint8_t> GalacticSession::Impl::encode_section(const StateSection section) const {
    std::vector<std::uint8_t> bytes;
    switch (section) {
    case StateSection::campaign_scoring:
        detail::encode_score_ledger(bytes, score_ledger);
        break;
    case StateSection::ability_host:
        if (ability_host) { detail::encode_ability_setup(bytes, *setup.ability_host); detail::encode_ability_state(bytes, *ability_host); }
        break;
    case StateSection::session:
        detail::put_u64(bytes, setup.seed);
        detail::put_bytes(bytes, setup.content_identity);
        detail::put_bytes(bytes, setup.campaign_identity);
        detail::put_u64(bytes, completed_tick);
        detail::put_u32(bytes, setup.rules.fiscal_cycle_frames);
        detail::put_u64(bytes, clock.galactic_frame);
        detail::put_u64(bytes, clock.tactical_frame);
        detail::put_u32(bytes, clock.pause_depth);
        detail::put_u64(bytes, clock.fiscal_start);
        detail::put_u64(bytes, clock.fiscal_end);
        detail::put_u64(bytes, clock.fiscal_cycle);
        // The speed settings are pacing only (WGM-U02): session state, never hashed.
        detail::put_u64(bytes, next_id);
        detail::put_u8(bytes, setup.debug_commands ? 1U : 0U);
        detail::put_u32(bytes, static_cast<std::uint32_t>(seats.size()));
        for (const auto& row : seats) {
            detail::put_u32(bytes, row.seat_id.value);
            detail::put_u32(bytes, row.player);
            detail::put_u8(bytes, static_cast<std::uint8_t>(row.kind));
            detail::put_u32(bytes, row.lobby_slot);
            detail::put_u8(bytes, row.active ? 1U : 0U);
            detail::put_u32(bytes, row.human_side_id);
        }
        if (seat_control) {
            detail::put_u16(bytes, 5); // coordinator-reserved, opt-in policy tail
            detail::encode_seat_control_state(bytes, *seat_control);
        }
        break;
    case StateSection::players:
        detail::encode_players(bytes, setup.players);
        if (economy) detail::encode_player_economy(bytes, *economy);
        if (setup.fiscal) detail::encode_fiscal(bytes, *setup.fiscal);
        break;
    case StateSection::planets: {
        detail::put_u32(bytes, static_cast<std::uint32_t>(planets.size()));
        for (const auto& planet : sorted_planets()) detail::encode_planet(bytes, planet);
        break;
    }
    case StateSection::routes: {
        detail::put_u32(bytes, static_cast<std::uint32_t>(routes.size()));
        for (const auto& route : sorted_routes()) detail::encode_route(bytes, route);
        break;
    }
    case StateSection::graph: {
        detail::put_i64(bytes, setup.rules.maximum_fleet_movement_distance->raw());
        detail::put_u32(bytes, static_cast<std::uint32_t>(routes.size()));
        for (const auto& route : sorted_routes()) {
            detail::put_u64(bytes, route.id);
            detail::put_i64(bytes, route.hs_speed_factor.raw());
        }
        break;
    }
    case StateSection::victory:
        detail::encode_victory(bytes, *victory);
        break;
    case StateSection::control:
        detail::encode_control(bytes, *control);
        break;
    case StateSection::movement:
        if (movement_inputs) detail::encode_movement(bytes, *movement_inputs, sorted_fleets());
        break;
    case StateSection::fleet_control:
        detail::encode_fleet_control(bytes, fleet_control_state(), sorted_fleets());
        break;
    case StateSection::census: {
        auto live = *setup.census;
        live.objects.clear();
        for (const auto& [id, row] : actor_records) { (void)id; live.objects.push_back(row); }
        std::vector<std::uint8_t> encoded;
        detail::encode_census(encoded, live);
        detail::put_u32(bytes, static_cast<std::uint32_t>(encoded.size()));
        detail::put_bytes(bytes, encoded);
        detail::encode_population(bytes, population);
        break;
    }
    case StateSection::observer:
        detail::encode_observer(bytes, *observer);
        break;
    case StateSection::planet_access:
        detail::encode_planet_access(bytes, *planet_access);
        break;
    case StateSection::production:
        detail::encode_production(bytes, *production);
        break;
    case StateSection::income:
        detail::encode_income_state(bytes, *income);
        break;
    case StateSection::economy_corruption:
        detail::encode_economy_corruption(bytes, *economy_corruption);
        break;
    case StateSection::native_invasions:
        detail::put_u64(bytes, next_native_invasion);
        detail::put_u64(bytes, native_invasions.size());
        for (const auto& [id, row] : native_invasions) {
            detail::put_u64(bytes, id); detail::put_u8(bytes, static_cast<std::uint8_t>(row.state));
            detail::put_u32(bytes, row.invader); detail::put_u32(bytes, row.defender);
            detail::put_u64(bytes, row.fleet); detail::put_u64(bytes, row.planet);
            detail::put_u64(bytes, row.conflict); detail::put_u8(bytes, row.success);
            detail::put_u8(bytes, row.reader_released);
            detail::put_u64(bytes, row.transports.size());
            for (const auto& transport : row.transports) { detail::put_u64(bytes, transport.id); detail::put_u64(bytes, transport.type); }
        }
        detail::put_u64(bytes, native_invasion_queue.size());
        for (const auto id : native_invasion_queue) detail::put_u64(bytes, id);
        break;
    case StateSection::fleets: {
        detail::put_u32(bytes, static_cast<std::uint32_t>(fleets.size()));
        for (const auto& fleet : sorted_fleets()) detail::encode_fleet(bytes, fleet);
        break;
    }
    case StateSection::outcomes: {
        detail::put_u32(bytes, static_cast<std::uint32_t>(applied_outcomes));
        for (std::size_t i = 0; i < applied_outcomes; ++i) {
            const auto& entry = setup.outcomes[i];
            detail::put_u64(bytes, entry.completed_tick); detail::put_u64(bytes, entry.conflict);
            detail::put_u32(bytes, static_cast<std::uint32_t>(entry.payload.size()));
            detail::put_bytes(bytes, entry.payload);
        }
        break;
    }
    }
    return bytes;
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
        const auto humans = std::count_if(seats.begin(), seats.end(), [](const Seat& row) { return row.kind == SeatKind::human; });
        const bool allowed = issuer && issuer->active && issuer->kind == SeatKind::human
            && (!(save->quick || save->autosave) || humans == 1)
            && (!save->prebattle || (clock.pause_depth == 0 && !pending_local_conflict));
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
    if (std::holds_alternative<PauseGalaxy>(command.payload)) {
        events.push_back(event);
        // WGM-05: nested; only the first pause has first-pause effects.
        ++clock.pause_depth;
        session_dirty();
        if (clock.pause_depth == 1) {
            event.kind = GalacticEventKind::galaxy_paused;
            events.push_back(event);
        }
        return;
    }
    if (std::holds_alternative<ResumeGalaxy>(command.payload)) {
        // WGM-06: a resume needs a positive counter; only the decrement to zero resumes.
        if (clock.pause_depth == 0) return reject(GalacticRejectReason::not_paused);
        events.push_back(event);
        --clock.pause_depth;
        session_dirty();
        if (clock.pause_depth == 0) {
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

void GalacticSession::Impl::refresh_credit_caps() {
    // GFD-05: ownership commits clamp accounts immediately, before fiscal collection.
    if (!setup.fiscal) return;
    // WGEP-17/60: counts are maintained by ownership commits, avoiding a planet
    // scan per player. This bounded player-list commit refreshes even zero owners.
    for (auto& row : economy->players) {
        const auto count = math::Fixed::from_integer(static_cast<std::int64_t>(std::max(std::size_t{1}, owned_planet_counts[row.player]))).value();
        row.credit_cap = math::multiply(count, setup.fiscal->credit_cap_per_planet).value();
        row.credits = std::min(row.credits, row.credit_cap);
    }
    dirty[static_cast<std::size_t>(StateSection::players)] = true;
}

std::span<const CensusObject> GalacticSession::Impl::actor_inputs() const {
    if (!published_actor_records) {
        std::vector<CensusObject> records; records.reserve(actor_records.size());
        for (const auto& [id, row] : actor_records) { (void)id; records.push_back(row); }
        published_actor_records = std::make_shared<const std::vector<CensusObject>>(std::move(records));
    }
    return *published_actor_records;
}
std::shared_ptr<const GalacticSnapshot> GalacticSession::Impl::publish_native_snapshot(std::vector<GalacticEvent> events) const {
    if (native_commit_publication) {
        ++native_commit_publications;
        NativeSnapshotChanges changes;
        // Cancellation has a refund publication before its removal event.
        // The staged final columns cannot supply this intermediate event prefix.
        changes.events = std::move(events);
        return GalacticSnapshot::native_view(native_commit_publication, std::move(changes));
    }
    return make_snapshot(std::move(events));
}
std::shared_ptr<const GalacticSnapshot> GalacticSession::Impl::make_snapshot(std::vector<GalacticEvent> events,
    const PartitionExecutor* executor) const {
    gc_profile::Scope profile_scope(gc_profile::Phase::make_snapshot);
    gc_profile::Scope context_scope(gc_profile::Phase::snapshot_other, gc_profile::Phase::snapshot_ai);
    std::optional<gc_profile::Scope> part(std::in_place, gc_profile::Phase::snap_sorted);
    refresh_economy_input_generation();
    auto live_fleets = sorted_fleets();
    part.emplace(gc_profile::Phase::snap_observer);
    if (observer && observer_projection_generation != observer_input_generation) {
        observer_projection = std::make_shared<const std::vector<ObservedPlanet>>(
            observed_planets(*observer, live_fleets, movement_inputs.get(), actor_inputs()));
        observer_projection_generation = observer_input_generation;
        observer_projection_records += observer->records.size();
    }
    part.emplace(gc_profile::Phase::snap_menus);
    auto published = clock;
    // WGEP-140/141: rebuild derived copied menus only when producer inputs change.
    // Reuse the same immutable vector across idle ticks and render frames.
    math::Fixed local_credits;
    std::uint64_t local_tech{};
    if (observer && economy) for (const auto& account : economy->players)
        if (account.player == observer->local_player) { local_credits = account.credits; local_tech = static_cast<std::uint64_t>(account.current_tech); break; }
    const std::array<std::uint64_t, 5> market_key{ability_host ? ability_input_generation : economy_input_generation->accounts,
        observer_input_generation, observer ? observer->local_player : 0,
        static_cast<std::uint64_t>(local_credits.raw()), local_tech};
    if (!published_black_market_menus || black_market_publication_key != market_key) {
        if (ability_host) ++ability_work.publications;
        auto menus =
            economy_corruption && economy && observer && observer_projection
                ? observed_black_market_menus(*economy_corruption, *economy, observer->local_player, *observer_projection)
                : std::vector<ObservedBlackMarketMenu>{};
        if (ability_host) for (auto& menu : menus) {
            const auto at = std::lower_bound(ability_host->preparations.begin(), ability_host->preparations.end(), menu.buyer,
                [](const auto& row, auto id) { return row.actor < id; });
            if (at != ability_host->preparations.end() && at->actor == menu.buyer) menu.preparation = at->id;
        }
        published_black_market_menus = std::make_shared<const std::vector<ObservedBlackMarketMenu>>(std::move(menus));
        std::vector<AbilityResult> results;
        if (ability_host && observer && observer_projection) for (const auto& result : ability_host->results) {
            if (result.player == observer->local_player && std::any_of(observer_projection->begin(), observer_projection->end(),
                [&](const auto& row) { return row.player == result.player && row.planet == result.planet && row.discovered; }))
                results.push_back(result); // coarse outcome only: no hidden refusal cause
        }
        published_ability_results = std::make_shared<const std::vector<AbilityResult>>(std::move(results));
        published_hero_action_menus.reset();
        published_corruption_menu = publish_corruption_menu();
        if (ability_host && observer && observer_projection) for (const auto& menu : ability_host->preparations) {
            if (!menu.open || (menu.kind != AbilityKind::technology_theft && menu.kind != AbilityKind::superweapon_hack
                && menu.kind != AbilityKind::planet_destruction)) continue;
            const auto actor = actor_records.find(menu.actor);
            if (actor == actor_records.end() || actor->second.owner != observer->local_player || actor->second.dead || actor->second.delete_pending) continue;
            if (std::none_of(observer_projection->begin(), observer_projection->end(), [&](const auto& row) {
                return row.player == observer->local_player && row.planet == menu.planet && row.discovered;
            })) continue;
            HeroActionMenus value; value.player = observer->local_player; value.actor = menu.actor;
            value.planet = menu.planet; value.preparation = menu.id;
            if (menu.kind == AbilityKind::technology_theft) value.theft.emplace();
            if (economy) for (const auto& account : economy->players) if (account.player == value.player
                && account.current_tech >= 0 && account.current_tech <= std::numeric_limits<unsigned>::max())
                value.tech = static_cast<unsigned>(account.current_tech);
            if (menu.kind == AbilityKind::planet_destruction) {
                // WGCH-151/155: the supplied source and planet; refusal is an execution fact, never a preview gate.
                if (menu.actor > std::numeric_limits<hero_actions::Id>::max()) continue; // 32-bit display payload (named seam)
                const auto source = static_cast<hero_actions::Id>(menu.actor);
                value.destruction = DestructionPreparation{source, menu.planet, hero_actions::Refusal::none, source,
                    prepared_ability(menu, 0)};
            } else if (menu.kind == AbilityKind::superweapon_hack) {
                // WGCH-66/112: the weapon identity is display-only; affordability compares the local credits.
                const auto weapon = find_hack_weapon(menu.planet); const auto prepared = prepared_ability(menu, 0);
                if (!weapon || !setup.ability_host) continue;
                // The display payload is 32-bit; an identity that does not fit is never published (named seam).
                constexpr std::uint64_t limit = std::numeric_limits<hero_actions::Id>::max();
                if (menu.actor > limit || menu.planet > limit || weapon->id > limit || weapon->type > limit
                    || setup.ability_host->hack_required_type > limit) continue;
                value.hack = ObservedHackChoice{{static_cast<hero_actions::Id>(menu.actor), static_cast<hero_actions::Id>(menu.planet),
                    static_cast<hero_actions::Id>(setup.ability_host->hack_required_type), prepared.price},
                    static_cast<hero_actions::Id>(weapon->id), static_cast<hero_actions::Id>(weapon->type),
                    local_credits >= prepared.price, prepared};
            } else for (std::uint32_t i = 0; i < menu.theft_choices.size(); ++i) {
                auto prepared = prepared_ability(menu, i);
                value.theft->push_back({i, {}, local_credits >= prepared.price, prepared});
            }
            published_hero_action_menus = std::make_shared<const HeroActionMenus>(std::move(value));
            break; // ordinary modal arbitration follows retained actor identity order
        }
        black_market_publication_key = market_key;
    }
    published.session_tick = completed_tick;
    part.emplace(gc_profile::Phase::snap_economy_input);
    const auto current_planets = sorted_planets();
    if (published_economy_input_generation != economy_input_generation) {
        auto input = *economy_input_generation; input.income.reset();
        if (setup.fiscal) {
            InlineExecutor startup_executor;
            const auto& workers = executor ? *executor : static_cast<const PartitionExecutor&>(startup_executor);
            if (auto values = forecast_contributions(workers, current_planets); values) {
                input.income.emplace();
                for (std::size_t i = 0; i < values.value().size(); ++i)
                    input.income->emplace_back(setup.fiscal->planets[i].planet, values.value()[i].total);
            }
        }
        economy_input_generation = std::make_shared<const EconomyInputGeneration>(std::move(input));
        published_economy_input_generation = economy_input_generation;
    }
    part.emplace(gc_profile::Phase::snap_forecast);
    std::optional<EconomyForecast> forecast;
    if (setup.fiscal && observer) {
        InlineExecutor startup_executor;
        const auto& workers = executor ? *executor : static_cast<const PartitionExecutor&>(startup_executor);
        const auto& fiscal = *setup.fiscal;
        const auto player = std::lower_bound(fiscal.players.begin(), fiscal.players.end(), observer->local_player,
            [](const auto& row, PlayerId id) { return row.player < id; });
        auto contributions = forecast_contributions(workers, current_planets); // cached pure fiscal provider
        if (player != fiscal.players.end() && player->player == observer->local_player && contributions) {
            EconomyForecast value;
            value.player = player->player;
            if (fiscal.contributions) { value.trade_routes = math::Fixed{}; value.alignment = math::Fixed{}; }
            if (fiscal.contributions && control) value.political = math::Fixed{};
            bool complete = true;
            for (std::size_t i = 0; complete && i < fiscal.planets.size(); ++i) {
                const auto& row = contributions.value()[i];
                value.planets.push_back({fiscal.planets[i].planet, planet_base_credits(i), row.total, {}, {}});
                value.planets.back().trade = fiscal.contributions ? std::optional(row.trade) : std::nullopt;
                if (!player->earns_planet_income || current_planets[i].owner != player->player) continue;
                complete = math::try_add(value.base, row.base, value.base)
                    && math::try_add(value.bonus, row.bonus, value.bonus)
                    && math::try_add(value.gross_total, row.total, value.gross_total);
                if (value.trade_routes) complete = complete && math::try_add(*value.trade_routes, row.trade, *value.trade_routes);
                if (value.alignment) complete = complete && math::try_add(*value.alignment, row.alignment, *value.alignment);
                if (value.political) complete = complete && math::try_add(*value.political, row.political, *value.political);
            }
            if (complete && economy_corruption) {
                auto copied = *economy_corruption; // pure preview, never consumes piracy
                std::vector<math::Fixed> inputs;
                for (const auto& row : contributions.value()) inputs.push_back(row.total);
                auto preview = corruption_payments(copied, current_planets, fiscal.players, inputs,
                    clock.galactic_frame, workers, false);
                if (!preview) complete = false;
                else {
                    const auto gross = preview.value()[static_cast<std::size_t>(player - fiscal.players.begin())];
                    auto extra = math::subtract(gross, value.gross_total);
                    if (!extra) complete = false;
                    else { value.corruption = extra.value(); value.gross_total = gross; }
                }
            }
            if (complete) {
                // GREF-13: retain each admitted source before presentation rounding.
                // This read-only publication neither samples income nor changes its cadence.
                publish_income_modifiers(value, current_planets);
                if (economy) for (const auto& account : economy->players) {
                    if (account.player != value.player) continue;
                    auto adjusted = adjusted_credits(account.credits, value.gross_total,
                        player->positive_credit_multiplier, account.credit_cap);
                    if (adjusted) {
                        auto delta = math::subtract(adjusted.value(), account.credits);
                        if (delta) value.adjusted_payment = delta.value();
                    }
                }
                forecast = std::move(value);
            }
        }
    }
    // WGP-19/47: copy committed state only after observer admission. Timers are
    // retained values, so idle ticks share storage while the reader computes age.
    part.emplace(gc_profile::Phase::snap_corruption);
    const std::array<std::uint64_t, 3> corruption_key{ability_input_generation,
        observer_input_generation, observer ? observer->local_player : 0};
    if (!published_corruption_transitions || corruption_publication_key != corruption_key) {
        std::vector<LiveCorruptionTransition> transitions;
        if (ability_host && observer && observer_projection) {
            std::set<PlanetId> discovered;
            for (const auto& seen : *observer_projection)
                if (seen.player == observer->local_player && seen.discovered) discovered.insert(seen.planet);
            for (const auto& row : ability_host->transitions) {
                if (discovered.contains(row.planet)) transitions.push_back(row);
            }
        }
        published_corruption_transitions = std::make_shared<const std::vector<LiveCorruptionTransition>>(std::move(transitions));
        corruption_publication_key = corruption_key;
    }
    // Offer admission consumes this publication's current projection; it must
    // not bypass the generation cache with a second full observer projection.
    part.emplace(gc_profile::Phase::snap_offers);
    const auto observations_for_offers = observer_projection
        ? std::span<const ObservedPlanet>(*observer_projection) : std::span<const ObservedPlanet>{};
    // The rows read the sections counted by offer_generation (the dirty flags the cached state hash trusts), the
    // counters below and the exact values in ProductionOfferInputs; anything else that moves (fleet positions in
    // transit, observer records) leaves them unchanged. Hash verification recomputes every reused result.
    const std::array<std::uint64_t, 5> offer_key{offer_generation, ability_input_generation, next_id, live_object_count,
        observer ? observer->local_player : 0};
    InlineExecutor offer_inline_executor;
    const auto& offer_workers = executor ? *executor : static_cast<const PartitionExecutor&>(offer_inline_executor);
    auto offer_inputs = production_offer_inputs(observations_for_offers, live_fleets);
    if (!published_production_offers || production_offer_key != offer_key || published_offer_inputs != offer_inputs) {
        published_production_offers = std::make_shared<const std::vector<ObservedProductionOffer>>(
            observed_production_offers(offer_inputs, live_fleets, offer_workers));
        production_offer_key = offer_key;
        published_offer_inputs = std::move(offer_inputs);
        ++production_offer_rebuilds;
    } else if (verify_cached_hash && *published_production_offers != observed_production_offers(offer_inputs, live_fleets, offer_workers)) {
        throw std::runtime_error("galactic cached production offers differ from an independent recomputation");
    }
    const auto offers = published_production_offers;
    (void)actor_inputs();
    if (!published_production_originals) {
        auto originals = production_originals;
        std::sort(originals.begin(), originals.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        published_production_originals = std::make_shared<const std::vector<CensusObject>>(std::move(originals));
    }
    if (setup.census && !published_sale_types)
        published_sale_types = std::make_shared<const std::vector<CensusType>>(setup.census->types);
    part.emplace(gc_profile::Phase::snap_construct);
    if (!published_production || published_production_generation != production_generation) {
        published_production = std::make_shared<const std::optional<ProductionSetup>>(production);
        published_production_generation = production_generation;
    } else if (verify_cached_hash && *published_production != production) {
        throw std::runtime_error("galactic published production differs from the live production state");
    }
    return std::make_shared<const GalacticSnapshot>(
        completed_tick, published, setup.players, current_planets, sorted_routes(), std::move(live_fleets), std::move(events), setup.rules.maximum_fleet_movement_distance, setup.fleet_control ? std::optional(fleet_control_state()) : std::nullopt, movement_inputs, seat_control, seats, economy, std::nullopt, control, control_effects, population, observer, income, victory, std::vector<ObservedProductionOffer>{}, observer_projection, std::move(forecast), published_production_originals, std::nullopt, std::nullopt, economy_corruption, planet_access, observer_input_generation, economy_input_generation, published_actor_records, published_black_market_menus, published_ability_results, published_hero_action_menus, published_corruption_menu, published_sale_types, published_corruption_transitions, offers, published_production);
}

GalacticSession::GalacticSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GalacticSession::GalacticSession(GalacticSession&&) noexcept = default;
GalacticSession& GalacticSession::operator=(GalacticSession&&) noexcept = default;
GalacticSession::~GalacticSession() = default;

core::Result<GalacticSession> GalacticSession::create(const GalacticSetup& setup) {
    if (auto valid = validate_setup(setup); !valid) return core::Result<GalacticSession>::failure(valid.error());
    auto impl = std::make_unique<Impl>();
    impl->verify_cached_hash = hash_verification_requested();
    impl->setup = setup;
    impl->native_population_schema = session_detail::native_population_schema(setup);
    if (setup.income) impl->native_income_schema = std::make_shared<const IncomeSetup>(*setup.income);
    if (setup.fiscal) impl->native_fiscal_schema = std::make_shared<const FiscalSetup>(*setup.fiscal);
    if (setup.census) for (const auto& row : setup.census->objects) impl->actor_records.emplace(row.id, row);
    impl->economy = setup.player_economy;
    impl->victory = setup.victory;
    // WGS-24: initialize the sparse leader projection once, in active object order.
    if (setup.census) for (const auto& row : setup.census->objects) impl->update_victory_actor(row);
    impl->control = setup.control;
    if (impl->control && impl->control->state.empty())
        for (const auto& planet : setup.planets) {
            PlanetControl row; row.planet = planet.id; row.current_player = planet.owner;
            impl->control->state.push_back(row);
        }
    impl->production = setup.production;
    impl->observer = setup.observer;
    if (setup.observer) {
        auto access = std::make_shared<PlanetAccessState>();
        for (const auto& planet : setup.planets) access->planets.push_back({planet.id, false, {}});
        impl->planet_access = std::move(access);
    }
    if (setup.income) impl->income = IncomeState{*setup.income, {}, 0, {}};
    impl->economy_corruption = setup.economy_corruption;
    if (setup.ability_host) {
        impl->ability_host.emplace();
        for (const auto& p : setup.ability_host->corruption_planets) {
            const auto at = std::lower_bound(impl->economy_corruption->planets.begin(), impl->economy_corruption->planets.end(), p.planet,
                [](const auto& row, auto id) { return row.planet < id; });
            const auto current = at != impl->economy_corruption->planets.end() && at->planet == p.planet ? at->corruption : corruption_fixture::Type::none;
            impl->ability_host->transitions.push_back({p.planet, {current, {}, false, false, 0, 0, 0}});
        }
        for (const auto& p : setup.ability_host->corruption_players) impl->ability_host->corruption_usage.push_back({p.player, {}});
        for (const auto& [id, row] : impl->actor_records) { (void)id; impl->refresh_ability_actor(row); }
    }
    impl->rebuild_technology_price_expiry();
    impl->expire_technology_prices();
    if (setup.movement) impl->movement_inputs = std::make_shared<const MovementSetup>(*setup.movement);
    impl->seats = setup.seats;
    if (setup.seat_control) impl->seat_control = SeatControlState{*setup.seat_control, {}};
    if (setup.rules.maximum_fleet_movement_distance) {
        auto graph = PlanetGraph::build(setup.planets, setup.routes, *setup.rules.maximum_fleet_movement_distance);
        if (!graph) return core::Result<GalacticSession>::failure(graph.error());
        impl->graph = std::move(graph).value();
    }
    // WGEP-59: opt-in fiscal windows begin at frame zero, cycle one.
    // Legacy clock-only fixtures retain their historical encoding.
    if (setup.fiscal) impl->clock.fiscal_cycle = 1;
    impl->clock.fiscal_end = setup.rules.fiscal_cycle_frames;
    EntityId highest = 0;
    for (const auto& planet : setup.planets) {
        impl->insert(planet);
        highest = std::max(highest, planet.id);
    }
    for (const auto& route : setup.routes) {
        impl->insert(route);
        highest = std::max(highest, route.id);
    }
    for (const auto& fleet : setup.fleets) {
        impl->insert(fleet);
        highest = std::max(highest, fleet.id);
        for (const auto& member : fleet.members) highest = std::max(highest, member.id);
    }
    impl->live_object_count = setup.planets.size() + setup.routes.size() + setup.fleets.size();
    for (const auto& fleet : setup.fleets) impl->live_object_count += fleet.members.size();
    if (setup.fleet_control) for (const auto& unit : setup.fleet_control->detached_units) {
        impl->detached_units.emplace(unit.member.id, unit);
        highest = std::max(highest, unit.member.id);
        ++impl->live_object_count;
    }
    if (setup.production) for (const auto& planet : setup.production->planets) for (const auto& object : planet.objects) {
        highest = std::max(highest, object.id);
        ++impl->live_object_count;
    }
    if (setup.census) {
        std::set<EntityId> existing;
        for (const auto& object : impl->sorted_objects()) existing.insert(object.first);
        for (const auto& fleet : setup.fleets) for (const auto& member : fleet.members) existing.insert(member.id);
        for (const auto& unit : setup.fleet_control->detached_units) existing.insert(unit.member.id);
        for (const auto& planet : setup.production->planets) for (const auto& object : planet.objects) existing.insert(object.id);
        if (setup.movement) for (const auto& row : setup.movement->contents) for (const auto& child : row.children) existing.insert(child.id);
        for (const auto& object : setup.census->objects) {
            highest = std::max(highest, object.id);
            if (existing.insert(object.id).second) ++impl->live_object_count;
        }
    }
    if (setup.movement) for (const auto& c : setup.movement->contents) for (const auto& child : c.children) {
        highest = std::max(highest, child.id); ++impl->live_object_count;
    }
    if (setup.control) for (const auto& planet : setup.control->planets) for (const auto& force : planet.landed) {
        highest = std::max(highest, force.id); ++impl->live_object_count;
    }
    impl->next_id = highest + 1;
    if (auto controlled = impl->initialize_control(); !controlled)
        return core::Result<GalacticSession>::failure(controlled.error());
    for (const auto& planet : impl->sorted_planets()) ++impl->owned_planet_counts[planet.owner];
    if (impl->control) impl->refresh_credit_caps();
    if (impl->control && impl->production) {
        std::vector<GalacticEvent> ignored_events;
        for (std::size_t i = 0; i < impl->control->planets.size(); ++i)
            impl->update_production_control(i, 0, ignored_events, false);
    }
    InlineExecutor inline_executor;
    if (auto initialized = impl->initialize_corruption(); !initialized) return core::Result<GalacticSession>::failure(initialized.error());
    if (impl->income) {
        // WGEP-166/167: object initialization, not a periodic ability service.
        for (const auto& source : impl->income->inputs.sources) if (source.planet_source)
            if (auto initialized = impl->refresh_planet_technology_price(source.planet); !initialized)
                return core::Result<GalacticSession>::failure(initialized.error());
        std::vector<GalacticEvent> ignored;
        if (auto initialized = impl->service_income(inline_executor, 0, ignored, false); !initialized)
            return core::Result<GalacticSession>::failure(initialized.error());
    }
    if (auto refreshed = impl->refresh_population(inline_executor); !refreshed) return core::Result<GalacticSession>::failure(refreshed.error());
    impl->dirty.fill(true);
    impl->initialize_observer();
    impl->snapshot = impl->make_snapshot({});
    const auto replay_fleet = [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) -> core::Result<void> {
        sim::detail::Reader input(effect.payload);
        PlayerId player{}; EntityId actor{}, target{}, expected_object{};
        std::uint8_t pending{}, admitted{}; std::uint16_t opcode{}, reason{}; std::uint32_t length{};
        if (!input.read_u32(player) || !input.read_u64(actor) || !input.read_u64(target)
            || !input.read_u8(pending) || pending > 1 || !input.read_u16(opcode)
            || !input.read_u32(length) || length > galactic_replay_max_payload || length > input.remaining())
            return fail<void>(diagnostic_codes::invalid_setup, "invalid native fleet journal framing");
        std::vector<std::uint8_t> body(length);
        if (!input.read_bytes(body) || !input.read_u8(admitted) || admitted > 1 || !input.read_u64(expected_object)
            || !input.read_u16(reason) || input.remaining())
            return fail<void>(diagnostic_codes::invalid_setup, "invalid native fleet journal receipt");
        core::Result<NativeFleetReceipt> result = core::Result<NativeFleetReceipt>::success({});
        if (effect.kind == NativeEffectKind::fleet) {
            if (actor || target) return fail<void>(diagnostic_codes::invalid_setup, "native fleet journal has extraneous identities");
            auto payload = decode_payload(CommandLayer::galactic, opcode, body);
            if (!payload || encode_payload(*payload) != body)
                return fail<void>(diagnostic_codes::invalid_setup, "invalid native fleet journal command");
            result = world.admit_native_fleet(player, *payload, pending != 0);
        } else {
            if (opcode || !body.empty()) return fail<void>(diagnostic_codes::invalid_setup, "native containment journal has extraneous command bytes");
            if (effect.kind == NativeEffectKind::launch) result = world.launch_native_transport(player, actor, target, pending != 0);
            else if (effect.kind == NativeEffectKind::land && !target) result = world.land_native_transports(player, actor, pending != 0);
            else if (effect.kind == NativeEffectKind::actor_land && target) {
                const auto landed = world.land_native_transport(player, actor, target, pending != 0);
                if (!landed) return core::Result<void>::failure(landed.error());
                result = core::Result<NativeFleetReceipt>::success(landed.value().event);
            }
            else if (effect.kind == NativeEffectKind::orbit_recheck && !target && !pending) {
                auto checked = world.recheck_native_orbit(player, actor);
                if (!checked) return checked;
                result = core::Result<NativeFleetReceipt>::success({true, actor});
            } else return fail<void>(diagnostic_codes::invalid_setup, "invalid native fleet journal owner");
        }
        if (!result) return core::Result<void>::failure(result.error());
        if (result.value().admitted != (admitted != 0) || result.value().object != expected_object
            || static_cast<std::uint16_t>(result.value().reason) != reason)
            return fail<void>(diagnostic_codes::invalid_setup, "native fleet reconstruction differs from its recorded receipt");
        return core::Result<void>::success();
    };
    for (const auto kind : {NativeEffectKind::fleet, NativeEffectKind::launch, NativeEffectKind::land, NativeEffectKind::orbit_recheck, NativeEffectKind::actor_land})
        impl->native_replayers.emplace(kind, replay_fleet);
    impl->native_replayers.emplace(NativeEffectKind::invasion, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        return world.impl_->replay_native_invasion(world, effect);
    });
    impl->native_replayers.emplace(NativeEffectKind::ability_lifecycle, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        sim::detail::Reader in(effect.payload); EntityId actor{}; std::uint8_t operation{};
        if (!in.read_u8(operation) || operation > 2 || !in.read_u64(actor) || !actor)
            return fail<void>(diagnostic_codes::invalid_setup, "invalid ability lifecycle journal");
        if (operation == 2) {
            std::uint8_t enabled{};
            if (!in.read_u8(enabled) || enabled > 1 || in.remaining())
                return fail<void>(diagnostic_codes::invalid_setup, "invalid contested-space journal");
            return world.set_destruction_contested_check(actor, enabled != 0);
        }
        if (operation == 1) {
            TypeId type{}, previous{};
            const auto before = world.impl_->actor_records.find(actor);
            if (!in.read_u64(type) || !type || !in.read_u64(previous) || in.remaining()
                || before == world.impl_->actor_records.end() || before->second.current_type != previous
                || !world.impl_->ability_host)
                return fail<void>(diagnostic_codes::invalid_setup, "invalid ability representation journal");
            return world.change_actor_current_type(actor, type);
        }
        std::uint8_t enabled{}, cancelled{}, kind{};
        if (!in.read_u8(enabled) || enabled > 1 || !in.read_u8(cancelled) || cancelled > 1
            || !in.read_u8(kind) || kind < 1 || kind > 5 || in.remaining())
            return fail<void>(diagnostic_codes::invalid_setup, "invalid ability service journal");
        return world.set_ability_lifecycle(actor, enabled != 0, cancelled != 0, static_cast<AbilityKind>(kind));
    });
    impl->native_replayers.emplace(NativeEffectKind::corruption_lifecycle, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        sim::detail::Reader in(effect.payload); std::uint8_t operation{}, type{}; std::uint64_t id{};
        if (!in.read_u8(operation) || operation > 1 || !in.read_u64(id) || !id || !in.read_u8(type))
            return fail<void>(diagnostic_codes::invalid_setup, "invalid corruption lifecycle journal");
        if (operation == 1) {
            if (type < 1 || type > 3 || in.remaining()) return fail<void>(diagnostic_codes::invalid_setup, "invalid corruption mission result journal");
            return world.finish_corruption_mission(id, static_cast<CorruptionMissionState>(type));
        }
        std::int64_t seconds{};
        if (type > 8 || !in.read_i64(seconds) || seconds < 0 || in.remaining()) return fail<void>(diagnostic_codes::invalid_setup, "invalid corruption transition journal");
        auto started = world.begin_corruption(id, static_cast<corruption_fixture::Type>(type), math::Fixed::from_raw(seconds));
        return started ? core::Result<void>::success() : core::Result<void>::failure(started.error());
    });
    impl->native_replayers.emplace(NativeEffectKind::planet_ruins, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        sim::detail::Reader in(effect.payload); std::uint8_t operation{}, destroy{}; std::uint64_t planet{};
        if (!in.read_u8(operation) || operation != 0 || !in.read_u64(planet) || !planet || !in.read_u8(destroy) || destroy > 1 || in.remaining())
            return fail<void>(diagnostic_codes::invalid_setup, "invalid planet ruins journal");
        auto applied = world.request_planet_ruins(planet, destroy != 0);
        if (!applied) return core::Result<void>::failure(applied.error());
        return applied.value() ? core::Result<void>::success()
            : fail<void>(diagnostic_codes::invalid_setup, "planet ruins reconstruction was refused");
    });
    impl->native_replayers.emplace(NativeEffectKind::technology, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        sim::detail::Reader in(effect.payload); TechnologyWrite write;
        std::uint8_t unlock{}, sound{}; std::int64_t previous{}, maximum{};
        if (!in.read_u32(write.player) || !in.read_i64(write.target) || !in.read_u8(unlock) || unlock > 1
            || !in.read_u8(sound) || sound > 1 || !in.read_i64(previous) || !in.read_i64(maximum) || in.remaining()
            || !world.impl_->economy || !world.impl_->production)
            return fail<void>(diagnostic_codes::invalid_setup, "invalid technology journal");
        const auto& accounts = world.impl_->economy->players;
        const auto account = std::find_if(accounts.begin(), accounts.end(), [&](const auto& row) { return row.player == write.player; });
        if (account == accounts.end() || account->current_tech != previous || account->maximum_tech != maximum)
            return fail<void>(diagnostic_codes::invalid_setup, "technology reconstruction differs from retained account");
        write.process_unlocks = unlock != 0; write.request_sound = sound != 0;
        auto written = world.set_technology(write);
        return written ? core::Result<void>::success() : core::Result<void>::failure(written.error());
    });
    impl->native_replayers.emplace(NativeEffectKind::tactical_built, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        sim::detail::Reader in(effect.payload); std::uint8_t operation{}; PlanetId planet{}; TacticalBuiltStructure record;
        std::int64_t x{}, y{}, z{}; PlayerId holder{};
        if (!in.read_u8(operation) || (operation != 1 && operation != 2) || !in.read_u64(planet) || !in.read_u64(record.type)
            || !in.read_i64(x) || !in.read_i64(y) || !in.read_i64(z) || !in.read_u32(record.enable_level)
            || !in.read_u32(record.owner) || !in.read_u32(holder) || in.remaining())
            return fail<void>(diagnostic_codes::invalid_setup, "invalid tactical persistence journal");
        record.position = {math::Fixed::from_raw(x), math::Fixed::from_raw(y), math::Fixed::from_raw(z)};
        if (operation == 1) return world.add_tactical_built_structure(planet, record, holder);
        if (record.enable_level || record.owner || holder || !world.snapshot()->control())
            return fail<void>(diagnostic_codes::invalid_setup, "invalid tactical removal journal");
        const auto& planets = world.snapshot()->control()->planets;
        const auto location = std::find_if(planets.begin(), planets.end(), [planet](const auto& p) { return p.planet == planet; });
        if (location == planets.end() || std::none_of(location->built_structures.begin(), location->built_structures.end(),
            [&](const auto& r) { return r.type == record.type && r.position == record.position; }))
            return fail<void>(diagnostic_codes::invalid_setup, "tactical removal reconstruction differs");
        return world.remove_tactical_built_structure(planet, record.type, record.position);
    });
    impl->native_replayers.emplace(NativeEffectKind::technology_price, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        return world.impl_->replay_technology_price(world, effect);
    });
    impl->native_replayers.emplace(NativeEffectKind::income_block, [](GalacticSession& world, const NativeEffect& effect, const PartitionExecutor&) {
        return world.impl_->replay_income_blocker(world, effect);
    });
    return core::Result<GalacticSession>::success(GalacticSession(std::move(impl)));
}

core::Result<GalacticSession> GalacticSession::from_replay(const GalacticReplay& replay) {
    if (auto valid = validate_replay(replay); !valid) return core::Result<GalacticSession>::failure(valid.error());
    auto created = create(replay.setup);
    if (!created) return created;
    for (const auto& command : replay.commands) {
        if (auto submitted = created.value().submit(command); !submitted) {
            return core::Result<GalacticSession>::failure(submitted.error());
        }
    }
    if (auto staged = created.value().stage_native_replay(replay.native_effects, replay.final_tick_count); !staged)
        return core::Result<GalacticSession>::failure(staged.error());
    return created;
}

core::Result<void> GalacticSession::Impl::journal_native_receipt(GalacticSession& world,
    NativeEffectKind kind, PlayerId player, EntityId actor, EntityId target, bool conflict_pending,
    const CommandPayload* command, const NativeFleetReceipt& receipt) {
    if (native_applying) return core::Result<void>::success();
    std::vector<std::uint8_t> payload;
    detail::put_u32(payload, player); detail::put_u64(payload, actor); detail::put_u64(payload, target);
    detail::put_u8(payload, conflict_pending ? 1 : 0); detail::put_u16(payload, command ? opcode_of(*command) : 0);
    const auto body = command ? encode_payload(*command) : std::vector<std::uint8_t>{};
    detail::put_u32(payload, static_cast<std::uint32_t>(body.size())); detail::put_bytes(payload, body);
    detail::put_u8(payload, receipt.admitted ? 1 : 0); detail::put_u64(payload, receipt.object);
    detail::put_u16(payload, static_cast<std::uint16_t>(receipt.reason));
    return world.append_native_effect(kind, std::move(payload));
}

core::Result<void> GalacticSession::append_native_effect(NativeEffectKind kind, std::vector<std::uint8_t> payload,
    NativeEffectPhase phase) {
    auto& impl = *impl_;
    if (impl.native_applying) return core::Result<void>::success();
    if (auto* shadow = native_shadow()) return stage_native_effect(*shadow, kind, std::move(payload), phase);
    if (impl.failed || impl.native_replaying || kind < NativeEffectKind::fleet || kind > NativeEffectKind::income_block
        || phase > NativeEffectPhase::world_service || payload.size() > galactic_replay_max_payload
        || impl.native_effects.size() + impl.executed.size() + impl.pending.size() >= galactic_max_commands
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
    auto& impl = *impl_;
    if (impl.failed || impl.completed_tick || !impl.native_effects.empty() || impl.native_replaying)
        return fail<void>(diagnostic_codes::invalid_setup, "native reconstruction requires a fresh world");
    GalacticReplay probe{impl.setup, final_tick, {}, std::vector<NativeEffect>(effects.begin(), effects.end())};
    if (auto valid = validate_replay(probe); !valid) return valid;
    impl.native_replay = std::move(probe.native_effects); impl.native_replay_cursor = 0;
    impl.native_replay_end = final_tick; impl.native_replaying = !impl.native_replay.empty();
    return core::Result<void>::success();
}
bool GalacticSession::replaying_native_effects() const noexcept { return impl_->native_replaying; }
std::span<const NativeEffect> GalacticSession::native_effects() const noexcept { return impl_->native_effects; }
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
        if (handler == impl.native_replayers.end()) return fail<void>(diagnostic_codes::invalid_setup, "native effect requires its typed reconstruction owner");
        struct Applying {
            bool& flag;
            explicit Applying(bool& value) : flag(value) { flag = true; }
            ~Applying() { flag = false; }
        } applying(impl.native_applying);
        auto applied = handler->second(*this, effect, executor);
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
    if (impl_->executed.size() + impl_->pending.size() >= galactic_max_commands) {
        return fail<void>(diagnostic_codes::resource_limit, context + ": session command count limit reached");
    }
    impl_->pending.emplace(key, command);
    impl_->last_submitted.insert_or_assign(key.seat, order);
    return core::Result<void>::success();
}

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
    std::optional<gc_profile::Scope> commands_scope(std::in_place, gc_profile::Phase::tick_commands);
    while (!impl.pending.empty() && impl.pending.begin()->first.tick == executing) {
        auto command = std::move(impl.pending.begin()->second);
        impl.pending.erase(impl.pending.begin());
        // GFS-01: admission reads state left by earlier synchronized commands.
        if (auto refreshed = impl.refresh_population(executor); !refreshed) {
            impl.failed = true; return core::Result<GalacticTick>::failure(refreshed.error());
        }
        impl.execute(command, executing, events, pending_local_conflict);
        if (impl.failed) return fail<GalacticTick>(diagnostic_codes::resource_limit, "credit victory admission failed during command commit");
        impl.executed.push_back(std::move(command));
    }

    commands_scope.reset();
    // GFD-23 / GFSC-13: discovery reads prior committed presence before movement.
    // WGUI-01: paused passes reset orbit counters; allied/corruption reveal still qualifies.
    if (auto result = gc_profile::timed(gc_profile::Phase::tick_discovery, [&] { return impl.service_discovery(executor, executing, events); }); !result) {
        impl.failed = true; return core::Result<GalacticTick>::failure(result.error());
    }
    tick.work.discovery_records = impl.discovery_records;
    tick.work.discovery_targets = impl.discovery_targets;

    // A paused galaxy runs no service: no phase, no frame, no fiscal or periodic check (WGM-05/06).
    const bool frame_admitted = impl.clock.pause_depth == 0;
    if (frame_admitted) {
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
            return fail<GalacticTick>(diagnostic_codes::worker_failure, "galactic-gather failed: " + phase.error().message);
        }
        tick.work.gathered_objects = gathered.size();
        stage_scope.emplace(gc_profile::Phase::tick_movement);
        // WGM-89: movement object service precedes fiscal processing.
        if (auto access = refresh_movement_access(executor); !access) {
            impl.failed = true; return core::Result<GalacticTick>::failure(access.error());
        }
        if (auto moved = impl.service_movement(gathered, executor, executing, events); !moved) {
            impl.failed = true;
            return core::Result<GalacticTick>::failure(moved.error());
        }
        if (impl.dirty[static_cast<std::size_t>(StateSection::fleets)]) impl.population_stale = true;
        stage_scope.emplace(gc_profile::Phase::tick_control);

        // GFD-02/05: committed ownership listeners and immediate credit-cap clamp
        // precede fiscal; its consumer must gather fresh post-control inputs.
        // GFD-01 / GFS-U01 retain arrival-to-control admission as a scheduling seam.
        if (auto completed = impl.service_corruption(executor, events.size()); !completed) {
            impl.failed = true; return core::Result<GalacticTick>::failure(completed.error());
        }
        if (auto controlled = impl.service_control(executor, executing, events, tick.work); !controlled) {
            impl.failed = true;
            return core::Result<GalacticTick>::failure(controlled.error());
        }
        if (auto access = refresh_movement_access(executor); !access) {
            impl.failed = true; return core::Result<GalacticTick>::failure(access.error());
        }
        if (impl.control_handler && !impl.control_effects.empty()) {
            const auto current_planets = impl.sorted_planets();
            auto applied = impl.control_handler(impl.control_effects, current_planets);
            if (!applied) { impl.failed = true; return core::Result<GalacticTick>::failure(applied.error()); }
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
            if (!balance) { impl.failed = true; return core::Result<GalacticTick>::failure(balance.error()); }
            auto delta = math::subtract(balance.value(), account->credits);
            if (!delta) { impl.failed = true; return core::Result<GalacticTick>::failure(delta.error()); }
            if (impl.credit_notification_handler) {
                auto notified = impl.credit_notification_handler({CreditNotification::Kind::windfall,
                    effect.new_owner, balance.value(), delta.value(), {}});
                if (!notified) { impl.failed = true; return core::Result<GalacticTick>::failure(notified.error()); }
            }
            account->credits = balance.value();
            impl.dirty[static_cast<std::size_t>(StateSection::players)] = true;
            // WGP-53: first-capture award, after crediting; not every windfall.
            if (impl.feedback_notice_handler && impl.observer && effect.new_owner == impl.observer->local_player)
                impl.feedback_notice_handler({GalacticFeedbackNotice::Kind::capture_reward, effect.new_owner, effect.planet, events.size()});
            if (auto tested = impl.test_credit_condition(effect.new_owner, executing, events); !tested) {
                impl.failed = true; return core::Result<GalacticTick>::failure(tested.error());
            }
        }
        // GFD-03/08: command purchases precede collection; production placement
        // commits before fresh fiscal reads. GFD-04 keeps all reads before timer advance.
        // GFS-19/GFC-09: object ability service precedes fiscal.
        // income_production_turn_order: gambling sample before production is
        // placed in the GFS-19 ability turn; RNG/source lifecycle remains GFS-U05.
        // Its order against production completion is unqualified.
        stage_scope.emplace(gc_profile::Phase::tick_income);
        if (auto serviced = impl.service_income(executor, executing, events); !serviced) {
            impl.failed = true;
            return fail<GalacticTick>(diagnostic_codes::worker_failure, serviced.error().message);
        }
        stage_scope.emplace(gc_profile::Phase::tick_production);
        if (auto serviced = impl.service_production(executor, executing, events, pending_local_conflict); !serviced) {
            impl.failed = true;
            return core::Result<GalacticTick>::failure(serviced.error());
        }
        tick.work.production_producers = impl.production_producers;
        tick.work.production_entries = impl.production_entries;
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
        auto& clock = impl.clock;
        // Legacy clock-only streams keep their frame convention. Configured fiscal
        // reads current mode time (WGEP-59/GFD-04). The conflict-aware mode manager
        // advances the active timer at its final outer tail (GFD-26/GFSC-18).
        if (step_timer && !impl.setup.fiscal && !impl.victory) ++clock.galactic_frame;
        // WGM-03, WGEP-01: one `if`, not a catch-up loop: at most one fiscal cycle per service. The
        // payment (WGEP-03..17) hooks in here with the fiscal ledger.
        const auto duration = impl.setup.rules.fiscal_cycle_frames;
        if (duration != 0 && clock.galactic_frame >= clock.fiscal_end) {
            gc_profile::Scope fiscal_scope(gc_profile::Phase::tick_fiscal);
            if (impl.setup.fiscal) {
                const auto& fiscal = *impl.setup.fiscal;
                // GFD-02/05/08: current owners and sources after political/production commits.
                const auto income_planets = impl.sorted_planets();
                auto contributions = impl.income_contributions(executor);
                if (!contributions) {
                    impl.failed = true;
                    return fail<GalacticTick>(diagnostic_codes::worker_failure, contributions.error().message);
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
                        values, clock.galactic_frame, executor, true);
                    if (!paid) { impl.failed = true; return core::Result<GalacticTick>::failure(paid.error()); }
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
                        return fail<GalacticTick>(diagnostic_codes::worker_failure, sum.error().message);
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
                        return fail<GalacticTick>(diagnostic_codes::worker_failure, "fiscal ledger: " + result.error().message);
                    }
                    if (ledger.players[i].credits != result.value()) {
                        ledger.players[i].credits = result.value();
                        impl.dirty[static_cast<std::size_t>(StateSection::players)] = true;
                    }
                    if (impl.credit_notification_handler) {
                        const auto seconds = math::Fixed::from_ratio(duration, 30);
                        if (!seconds) {
                            impl.failed = true; return core::Result<GalacticTick>::failure(seconds.error());
                        }
                        auto notified = impl.credit_notification_handler({CreditNotification::Kind::fiscal,
                            ledger.players[i].player, result.value(), totals[i], seconds.value()});
                        if (!notified) {
                            impl.failed = true; return core::Result<GalacticTick>::failure(notified.error());
                        }
                    }
                    // WGEP-16: each player's account callback precedes cycle increment.
                    if (totals[i].raw() != 0) {
                        if (auto v = impl.test_credit_condition(ledger.players[i].player, executing, events); !v) {
                            impl.failed = true; return core::Result<GalacticTick>::failure(v.error());
                        }
                    }
                }
            }
            if (impl.economy_corruption) {
                auto rolled = reroll_planet_income(*impl.economy_corruption, impl.setup.seed);
                if (!rolled) { impl.failed = true; return core::Result<GalacticTick>::failure(rolled.error()); }
                impl.income_forecast_cache.reset();
                impl.dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
            }
            clock.fiscal_start = clock.fiscal_end;
            clock.fiscal_end += duration;
            ++clock.fiscal_cycle;
            ++tick.work.fiscal_cycles;
            events.push_back({executing, GalacticEventKind::fiscal_cycle_completed, {0}, invalid_entity_id, 0,
                clock.fiscal_cycle, GalacticRejectReason::none});
        }
        // WGM-04: after fiscal work, the periodic checks on every frame divisible by 30 (leader-death
        // victory and destroyed-planet notices hook in here with their tickets).
        if (frame_admitted && clock.galactic_frame % periodic_check_frames == 0) ++tick.work.periodic_checks;
        if (impl.victory) {
            gc_profile::Scope victory_scope(gc_profile::Phase::tick_victory);
            // WGM-03/04: fiscal and leader callbacks are distinct admissions.
            if (tick.work.fiscal_cycles) {
                const std::array kinds{VictoryKind::cycles};
                if (auto v = impl.test_conditions(executor, kinds, executing, events); !v) {
                    impl.failed = true; return core::Result<GalacticTick>::failure(v.error());
                }
            }
            if (frame_admitted && clock.galactic_frame % periodic_check_frames == 0) {
                // WGS-171: this producer requests only kill-enemy-leader.
                const std::array kinds{VictoryKind::kill_leader};
                if (auto v = impl.test_conditions(executor, kinds, executing, events); !v) {
                    impl.failed = true; return core::Result<GalacticTick>::failure(v.error());
                }
            }
            // GFS-15/GFD-04: countdown after fiscal, before active-mode timer advance.
            if (!impl.victory_tail_deferred) impl.service_victory_tail(executing, events);
        }
        if ((impl.setup.fiscal || impl.victory) && step_timer && !impl.victory_tail_deferred) ++clock.galactic_frame;
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
        for (auto& planet : impl_->production->planets) {
            const auto count = std::erase_if(planet.objects, [&](const auto& object) { return dead(object.id); });
            impl_->live_object_count -= count; removed |= count != 0;
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
const GalacticSetup& GalacticSession::setup() const noexcept { return impl_->setup; }
std::span<const Seat> GalacticSession::seats() const noexcept { return impl_->seats; }
const std::optional<SeatControlState>& GalacticSession::seat_control() const noexcept { return impl_->seat_control; }
core::Result<void> GalacticSession::begin_battle_roles(const EntityId conflict, const PlayerId invader, const PlayerId defender) {
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
    impl_->seat_control->battle = BattleRoleChoice{conflict, human, ai, other->side, other->observer_ai ? ai : human, BattleTeamRole::pending};
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

GalacticReplay GalacticSession::record() const { return GalacticReplay{impl_->setup, impl_->completed_tick, impl_->executed, impl_->native_effects}; }
std::shared_ptr<const GalacticSnapshot> GalacticSession::snapshot() const noexcept { if (const auto* shadow = native_shadow()) return shadow->snapshot(); return impl_->snapshot; }

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

std::vector<std::uint8_t> GalacticSession::canonical_state_bytes() const {
    std::vector<std::uint8_t> bytes(detail::state_magic.begin(), detail::state_magic.end());
    detail::put_u32(bytes, detail::state_encoding_version);
    detail::put_u32(bytes, static_cast<std::uint32_t>(impl_->section_count()));
    for (const auto section : impl_->section_ids()) {
        const auto encoded = impl_->encode_section(static_cast<StateSection>(section));
        detail::put_u16(bytes, static_cast<std::uint16_t>(section));
        detail::put_u16(bytes, detail::section_version);
        detail::put_u64(bytes, encoded.size());
        detail::put_bytes(bytes, encoded);
    }
    return bytes;
}

void GalacticSession::Impl::refresh_economy_input_generation() const {
    const auto changed = [&](StateSection section) { return dirty[static_cast<std::size_t>(section)]; };
    // Inactive optional sections retain their initial dirty bit: only present
    // native owners contribute inputs to this derived publication.
    if (production && changed(StateSection::production)) ++production_generation;
    if (changed(StateSection::planets) || (production && changed(StateSection::production)) || changed(StateSection::players)
        || (setup.census && changed(StateSection::census)) || (economy_corruption && changed(StateSection::economy_corruption))
        || (ability_host && changed(StateSection::ability_host))) ++offer_generation;
    const bool census = changed(StateSection::planets) || (production && changed(StateSection::production))
        || changed(StateSection::fleets) || (setup.movement && changed(StateSection::movement))
        || (control && changed(StateSection::control)) || (setup.census && changed(StateSection::census))
        || (observer && changed(StateSection::observer));
    const bool accounts = census || changed(StateSection::players)
        || (income && changed(StateSection::income))
        || (economy_corruption && changed(StateSection::economy_corruption))
        || (ability_host && changed(StateSection::ability_host));
    if (!census && !accounts) return;
    auto next = *economy_input_generation;
    if (census) ++next.census;
    if (accounts) ++next.accounts;
    economy_input_generation = std::make_shared<const EconomyInputGeneration>(next);
}

StateHash GalacticSession::Impl::refresh_hash(GalacticTick::Work* work) {
    // Preserve publication invalidation before the ordinary hash consumes flags.
    refresh_economy_input_generation();
    bool changed = !cached_hash_present;
    for (const auto section : section_ids()) {
        if (!dirty[section]) {
            if (work) ++work->sections_reused;
            continue;
        }
        auto bytes = encode_section(static_cast<StateSection>(section));
        digests[section] = hasher ? hasher->hash(std::move(bytes)) : StateHash(core::sha256_hex(bytes));
        // Only the ordinary world step consumes these flags: it also uses
        // them to invalidate population preparation. Barrier reads must leave
        // native writes visible to that next service.
        if (work) dirty[section] = false;
        changed = true;
        if (work) ++work->sections_encoded;
    }
    if (!changed) { verify_hash(); return cached_hash; }
    if (hasher) {
        // Queue the composition after every changed section job. Capture each
        // generation by value; a later native write cannot change this result.
        auto others = digests;
        const auto ids = section_ids();
        cached_hash = hasher->derive(digests[0], [others, ids](const std::string& session_hex) {
            std::array<std::string, state_section_count> hex;
            hex[0] = session_hex;
            for (const auto section : ids) if (section != 0) hex[section] = others[section].get();
            return combine(hex, ids);
        });
    } else {
        std::array<std::string, state_section_count> hex;
        for (const auto section : section_ids()) hex[section] = digests[section].get();
        cached_hash = StateHash(combine(hex, section_ids()));
    }
    cached_hash_present = true;
    verify_hash();
    return cached_hash;
}
void GalacticSession::Impl::verify_hash() const {
    if (!verify_cached_hash) return;
    std::array<std::string, state_section_count> hex;
    for (const auto section : section_ids()) hex[section] = core::sha256_hex(encode_section(static_cast<StateSection>(section)));
    if (cached_hash.get() != combine(hex, section_ids())) throw std::runtime_error("galactic cached hash differs from independent full state hash");
}
StateHash GalacticSession::cached_state_hash() const { return impl_->refresh_hash(); }
void GalacticSession::set_hash_verification(bool enabled) noexcept { impl_->verify_cached_hash = enabled; }
std::string GalacticSession::state_sha256() const {
    std::array<std::string, state_section_count> hex;
    for (const auto section : impl_->section_ids()) {
        hex[section] = core::sha256_hex(impl_->encode_section(static_cast<StateSection>(section)));
    }
    return combine(hex, impl_->section_ids());
}

void GalacticSession::set_timeline_service(TimelineService service) { impl_->timeline_service = std::move(service); }
void GalacticSession::scramble_storage_for_testing() { impl_->rebuild(true); }

} // namespace eawr::sim::galactic
