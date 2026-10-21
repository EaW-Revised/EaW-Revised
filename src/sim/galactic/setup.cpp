#include "eawr/sim/galactic/events.hpp"
#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "eawr/sim/galactic/income.hpp"
#include "eawr/sim/galactic/economy_corruption.hpp"
#include "eawr/sim/galactic/ability_host.hpp"

#include "encoding.hpp"
#include "eawr/sim/galactic/movement.hpp"
#include "eawr/sim/galactic/control.hpp"
#include "eawr/sim/galactic/victory.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <set>
#include <string>

namespace eawr::sim::galactic {

namespace {

[[nodiscard]] core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, std::move(message)));
}

template <typename Item, typename Key>
[[nodiscard]] bool strictly_increasing(const std::vector<Item>& items, Key key) {
    return std::adjacent_find(items.begin(), items.end(),
               [&](const Item& left, const Item& right) { return !(key(left) < key(right)); })
        == items.end();
}

} // namespace

void update_seat_control_policy(SeatControlState& state, const std::span<const Seat> seats) {
    // WGS-P02/P04 (owner policy, #2115): keep a live lead, otherwise lowest active ID.
    for (auto& team : state.control.teams) {
        const auto live = [&](const Seat& seat) {
            return seat.kind == SeatKind::human && seat.active && seat.human_side_id == team.side;
        };
        if (std::none_of(seats.begin(), seats.end(), [&](const Seat& seat) { return live(seat) && seat.seat_id == team.lead; })) {
            const auto next = std::find_if(seats.begin(), seats.end(), live);
            team.lead = next == seats.end() ? SeatId{} : next->seat_id;
        }
    }
    for (auto& controller : state.control.controllers) {
        const bool human_roster = std::any_of(seats.begin(), seats.end(), [&](const Seat& seat) {
            return seat.kind == SeatKind::human && seat.player == controller.player;
        });
        if (human_roster) {
            const bool active = std::any_of(seats.begin(), seats.end(), [&](const Seat& seat) {
                return seat.kind == SeatKind::human && seat.active && seat.player == controller.player;
            });
            controller.kind = active ? FactionControl::human : controller.dropout;
        }
    }
    // WGS-P08 (owner policy, #2115): an empty choosing team has no readiness vote to await.
    if (state.battle && state.battle->role == BattleTeamRole::pending) {
        const auto team = std::find_if(state.control.teams.begin(), state.control.teams.end(), [&](const auto& value) {
            return value.side == state.battle->choosing_side;
        });
        if (team != state.control.teams.end() && team->lead.value == 0) state.battle->role = BattleTeamRole::spectate;
    }
}

core::Result<void> validate_setup(const GalacticSetup& setup) {
    if (setup.victory) { if (auto v = validate_victory(*setup.victory); !v) return v; }
    std::set<EntityId> outcome_conflicts;
    std::uint64_t outcome_tick{};
    std::uint64_t outcome_bytes{};
    if (setup.outcomes.size() > galactic_max_objects) return invalid("too many outcome journal entries");
    for (const auto& entry : setup.outcomes) {
        outcome_bytes += 20 + entry.payload.size();
        if (!entry.completed_tick || entry.completed_tick < outcome_tick || entry.completed_tick > galactic_max_ticks
            || !entry.conflict || !outcome_conflicts.insert(entry.conflict).second
            || entry.payload.empty() || entry.payload.size() > 1'048'576 || outcome_bytes > 67'108'864)
            return invalid("invalid outcome journal order or resource budget");
        outcome_tick = entry.completed_tick;
    }
    if (setup.players.empty() || setup.players.size() > galactic_max_players) {
        return invalid("a galaxy needs 1 to " + std::to_string(galactic_max_players) + " players");
    }
    if (setup.players.front().player_id == 0
        || !strictly_increasing(setup.players, [](const GalacticPlayer& player) { return player.player_id; })) {
        return invalid("player IDs must be nonzero and strictly increasing");
    }
    const auto listed_player = [&](const PlayerId id) {
        const auto found = std::lower_bound(setup.players.begin(), setup.players.end(), id,
            [](const GalacticPlayer& player, const PlayerId value) { return player.player_id < value; });
        return found != setup.players.end() && found->player_id == id;
    };
    if (setup.victory) {
        const auto& c = setup.victory->context;
        if (c.players.size() != setup.players.size() || c.planets.size() != setup.planets.size()) return invalid("victory metadata must cover every player and planet");
        for (std::size_t i = 0; i < c.players.size(); ++i)
            if (c.players[i].player != setup.players[i].player_id) return invalid("victory players must follow listed identities");
        for (std::size_t i = 0; i < c.planets.size(); ++i)
            if (c.planets[i].planet != setup.planets[i].id || !c.planets[i].type) return invalid("victory planets must follow listed identities");
    }
    if (setup.player_economy) {
        const auto& rows = setup.player_economy->players;
        if (rows.size() != setup.players.size()) return invalid("economy needs exactly one row per player");
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto& row = rows[index];
            if (row.player != setup.players[index].player_id)
                return invalid("economy rows must follow the listed player identities");
            const auto source_integer = [](std::int64_t value) {
                return value >= std::numeric_limits<std::int32_t>::min() && value <= std::numeric_limits<std::int32_t>::max();
            };
            if (!source_integer(row.current_tech) || !source_integer(row.maximum_tech))
                return invalid("player tech must fit the source integer");
        }
    }
    if (setup.fiscal) {
        const auto& f = *setup.fiscal;
        if (!setup.player_economy || setup.rules.fiscal_cycle_frames == 0
            || f.credit_cap_per_planet.raw() < 0 || f.players.size() != setup.players.size()
            || f.planets.size() != setup.planets.size()) return invalid("fiscal needs a ledger, window and complete source tables");
        // Bound all sums and cap products before a tick; hostile replay values cannot wrap.
        math::Fixed sum;
        for (std::size_t i = 0; i < f.planets.size(); ++i) {
            if (f.planets[i].planet != setup.planets[i].id || f.planets[i].credits.raw() < 0
                || !math::try_add(sum, f.planets[i].credits, sum)) return invalid("invalid or overflowing base income table");
        }
        math::Fixed largest_cap;
        const auto count = math::Fixed::from_integer(static_cast<std::int64_t>(std::max(std::size_t{1}, setup.planets.size())));
        if (!count || !math::try_multiply(count.value(), f.credit_cap_per_planet, largest_cap)) return invalid("overflowing fiscal credit cap");
        for (std::size_t i = 0; i < f.players.size(); ++i) {
            math::Fixed adjusted;
            if (f.players[i].player != setup.players[i].player_id || f.players[i].positive_credit_multiplier.raw() < 0
                || setup.player_economy->players[i].credit_cap.raw() < 0
                || !math::try_multiply(sum, f.players[i].positive_credit_multiplier, adjusted))
                return invalid("invalid fiscal player or overflowing difficulty income");
        }
    }
    if (setup.seats.size() > galactic_max_seats) {
        return invalid("a galaxy has at most " + std::to_string(galactic_max_seats) + " seats");
    }
    if ((!setup.seats.empty() && setup.seats.front().seat_id.value == 0)
        || !strictly_increasing(setup.seats, [](const Seat& seat) { return seat.seat_id; })) {
        return invalid("seat IDs must be nonzero and strictly increasing");
    }
    std::size_t human_seats = 0;
    std::set<HumanSideId> human_sides;
    for (const auto& seat : setup.seats) {
        if (!listed_player(seat.player)) {
            return invalid("seat " + std::to_string(seat.seat_id.value) + ": player " + std::to_string(seat.player) + " is not a player");
        }
        if (seat.kind != SeatKind::human && seat.kind != SeatKind::ai && seat.kind != SeatKind::script) {
            return invalid("seat " + std::to_string(seat.seat_id.value) + ": unknown seat kind");
        }
        if (seat.kind == SeatKind::human) {
            // Count the roster, including inactive seats that can reconnect. Explicit human
            // side membership is independent of owning player and faction type (ADR-020).
            if (seat.human_side_id == 0) return invalid("a human seat needs a nonzero human side ID");
            if (++human_seats > galactic_max_human_seats) {
                return invalid("a galaxy has at most " + std::to_string(galactic_max_human_seats) + " human seats");
            }
            human_sides.insert(seat.human_side_id);
            if (human_sides.size() > 2) {
                return invalid("human seats must share one coop side or exactly two versus sides");
            }
        } else if (seat.human_side_id != 0) {
            return invalid("a system issuer has no human side");
        }
    }
    if (setup.seat_control) {
        const auto& control = *setup.seat_control;
        if (control.teams.size() != human_sides.size() || control.controllers.size() != setup.players.size()
            || !strictly_increasing(control.teams, [](const auto& row) { return row.side; }))
            return invalid("seat policy must list every human side and faction once");
        std::set<PlayerId> human_players;
        for (const auto& team : control.teams) {
            if (!human_sides.contains(team.side)) return invalid("unknown human side in lead policy");
            PlayerId player{}; bool active{}, valid_lead{};
            for (const auto& seat : setup.seats) if (seat.human_side_id == team.side) {
                if (player != 0 && player != seat.player) return invalid("one human side must share one faction");
                player = seat.player;
                active |= seat.active;
                valid_lead |= seat.active && seat.seat_id == team.lead;
            }
            if (!human_players.insert(player).second || (active ? !valid_lead : team.lead.value != 0))
                return invalid("human sides need distinct factions and an active lead (zero only when empty)");
        }
        for (std::size_t i = 0; i < control.controllers.size(); ++i) {
            const auto& controller = control.controllers[i];
            const bool active = std::any_of(setup.seats.begin(), setup.seats.end(), [&](const auto& seat) {
                return seat.kind == SeatKind::human && seat.active && seat.player == controller.player;
            });
            const auto expected = human_players.contains(controller.player)
                ? (active ? FactionControl::human : controller.dropout) : FactionControl::ai;
            if (controller.player != setup.players[i].player_id || controller.kind != expected
                || (controller.dropout != FactionControl::ai && controller.dropout != FactionControl::passive)
                || controller.configuration.size() > 16384 || controller.state.size() > 16384)
                return invalid("invalid faction controller identity, kind or bounded persistent payload");
            if (std::any_of(setup.seats.begin(), setup.seats.end(), [&](const auto& seat) {
                return seat.kind == SeatKind::ai && seat.active && seat.player == controller.player;
            }) && active)
                return invalid("human-controlled factions reserve inactive AI fallback issuers");
        }
    }
    std::size_t objects = setup.planets.size() + setup.routes.size() + setup.fleets.size();
    for (const auto& fleet : setup.fleets) objects += fleet.members.size();
    if (setup.fleet_control) objects += setup.fleet_control->detached_units.size();
    if (objects > galactic_max_objects) {
        return invalid("a galaxy holds at most " + std::to_string(galactic_max_objects) + " objects");
    }
    if (!strictly_increasing(setup.planets, [](const Planet& planet) { return planet.id; })
        || !strictly_increasing(setup.routes, [](const Route& route) { return route.id; })
        || !strictly_increasing(setup.fleets, [](const Fleet& fleet) { return fleet.id; })) {
        return invalid("planets, routes and fleets must each be in strictly increasing ID order");
    }
    // One stable-ID space: every object ID is nonzero and distinct across all tables.
    std::set<EntityId> ids;
    const auto claim = [&](const EntityId id) {
        // Keep the allocator's next ID representable and nonzero.
        return id != invalid_entity_id && id != std::numeric_limits<EntityId>::max() && ids.insert(id).second;
    };
    for (const auto& planet : setup.planets) {
        if (!claim(planet.id)) return invalid("planet " + std::to_string(planet.id) + ": ID is zero or taken");
        if (!listed_player(planet.owner)) {
            return invalid("planet " + std::to_string(planet.id) + ": owner " + std::to_string(planet.owner) + " is not a player");
        }
    }
    const auto listed_planet = [&](const PlanetId id) {
        const auto found = std::lower_bound(setup.planets.begin(), setup.planets.end(), id,
            [](const Planet& planet, const PlanetId value) { return planet.id < value; });
        return found != setup.planets.end() && found->id == id;
    };
    if (setup.rules.maximum_fleet_movement_distance
        && (setup.rules.maximum_fleet_movement_distance->raw() < 0 || setup.planets.size() > 4096))
        return invalid("graph movement distance must be nonnegative and planet count at most 4096");
    for (const auto& route : setup.routes) {
        if (route.hs_speed_factor.raw() <= 0
            || (!setup.rules.maximum_fleet_movement_distance && route.hs_speed_factor.raw() != math::Fixed::scale))
            return invalid("route factor must be positive and requires graph rules when not one");
        if (!claim(route.id)) return invalid("route " + std::to_string(route.id) + ": ID is zero or taken");
        if (!listed_planet(route.from) || !listed_planet(route.to) || route.from == route.to) {
            return invalid("route " + std::to_string(route.id) + ": needs two distinct listed planets");
        }
    }
    for (const auto& fleet : setup.fleets) {
        if (!claim(fleet.id)) return invalid("fleet " + std::to_string(fleet.id) + ": ID is zero or taken");
        if (!listed_player(fleet.owner)) {
            return invalid("fleet " + std::to_string(fleet.id) + ": owner " + std::to_string(fleet.owner) + " is not a player");
        }
        if (!listed_planet(fleet.orbit)) {
            return invalid("fleet " + std::to_string(fleet.id) + ": orbit " + std::to_string(fleet.orbit) + " is not a planet");
        }
        if (fleet.raid_destination)
            return invalid("tick-zero fleets cannot seed a raid destination; qualified commands own it");
        if (fleet.orbit_slot > 2 || (!setup.fleet_control && fleet.orbit_slot != 0))
            return invalid("fleet slot requires fleet control and must be 0/1/2");
        if (fleet.members.empty()) return invalid("fleet " + std::to_string(fleet.id) + ": has no members");
    }
    for (const auto& fleet : setup.fleets) {
        for (const auto& member : fleet.members) {
            if (!claim(member.id)) {
                return invalid("fleet " + std::to_string(fleet.id) + ": member " + std::to_string(member.id)
                    + " is zero or taken");
            }
        }
    }
    if (setup.fleet_control) {
        if (!setup.rules.maximum_fleet_movement_distance) return invalid("fleet control requires graph rules");
        const auto& control = *setup.fleet_control;
        if (control.types.size() > galactic_max_objects
            || !strictly_increasing(control.types, [](const auto& row) { return row.type; })
            || (!control.types.empty() && control.types.front().type == 0)) return invalid("invalid fleet type traits");
        const auto known_type = [&](const TypeId type) {
            const auto found = std::lower_bound(control.types.begin(), control.types.end(), type,
                [](const auto& row, const auto id) { return row.type < id; });
            return found != control.types.end() && found->type == type;
        };
        if (!strictly_increasing(control.detached_units, [](const auto& row) { return row.member.id; }))
            return invalid("detached units must be in ID order");
        for (const auto& unit : control.detached_units)
            if (!claim(unit.member.id) || !listed_player(unit.owner) || !listed_planet(unit.planet) || !known_type(unit.member.type))
                return invalid("invalid detached unit identity, owner, planet or type");
        for (const auto& fleet : setup.fleets) for (const auto& member : fleet.members)
            if (!known_type(member.type)) return invalid("fleet member has no original-type traits");
    }
    if (auto valid = validate_movement_setup(setup); !valid) return valid;
    if (auto valid = validate_control_setup(setup); !valid) return valid;
    if (auto valid = validate_production(setup); !valid) return valid;
    if (auto valid = validate_income(setup); !valid) return valid;
    if (auto valid = validate_economy_corruption(setup); !valid) return valid;
    if (auto valid = validate_ability_host(setup); !valid) return valid;
    if (auto valid = validate_planet_income_rules(setup); !valid) return valid;
    if (auto valid = validate_census(setup); !valid) return valid;
    return validate_observer(setup);
}

core::Result<math::Fixed> adjusted_credits(const math::Fixed balance, math::Fixed delta,
    const math::Fixed positive_multiplier, const math::Fixed cap) {
    if (cap.raw() < 0 || positive_multiplier.raw() < 0)
        return core::Result<math::Fixed>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "negative account cap or multiplier"));
    if (delta.raw() > 0) {
        auto adjusted = math::multiply(delta, positive_multiplier);
        if (!adjusted) return adjusted;
        delta = adjusted.value();
    }
    auto total = math::add(balance, delta);
    if (!total) return total;
    return core::Result<math::Fixed>::success(std::clamp(total.value(), math::Fixed{}, cap));
}

core::Result<std::uint32_t> fiscal_cycle_frames(const math::Fixed seconds) {
    // seconds x 30 in Q24, rounded to the nearest frame with half ties away from zero (positive here).
    constexpr std::int64_t half = std::int64_t{1} << (math::Fixed::fractional_bits - 1);
    const auto raw = seconds.raw();
    if (raw <= 0 || raw > (std::int64_t{1} << 55)) {
        return core::Result<std::uint32_t>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "a fiscal cycle needs a positive duration of at most 2^31 seconds"));
    }
    const auto frames = (raw * galactic_frames_per_second + half) >> math::Fixed::fractional_bits;
    if (frames <= 0 || frames > std::int64_t{0xffffffff}) {
        return core::Result<std::uint32_t>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "the fiscal cycle does not fit a frame count"));
    }
    return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(frames));
}

std::string_view to_string(const GalacticEventKind kind) noexcept {
    switch (kind) {
    case GalacticEventKind::command_executed: return "command_executed";
    case GalacticEventKind::gambling_income_sampled: return "gambling_income_sampled";
    case GalacticEventKind::black_market_purchased: return "black_market_purchased";
    case GalacticEventKind::ability_prepared: return "ability_prepared";
    case GalacticEventKind::ability_settled: return "ability_settled";
    case GalacticEventKind::ability_cancelled: return "ability_cancelled";
    case GalacticEventKind::command_rejected: return "command_rejected";
    case GalacticEventKind::planet_owner_changed: return "planet_owner_changed";
    case GalacticEventKind::fleet_orbit_changed: return "fleet_orbit_changed";
    case GalacticEventKind::fiscal_cycle_completed: return "fiscal_cycle_completed";
    case GalacticEventKind::galaxy_paused: return "galaxy_paused";
    case GalacticEventKind::galaxy_resumed: return "galaxy_resumed";
    case GalacticEventKind::speed_changed: return "speed_changed";
    case GalacticEventKind::save_requested: return "save_requested";
    case GalacticEventKind::save_rejected: return "save_rejected";
    case GalacticEventKind::fleet_created: return "fleet_created";
    case GalacticEventKind::fleet_destroyed: return "fleet_destroyed";
    case GalacticEventKind::member_attached: return "member_attached";
    case GalacticEventKind::member_detached: return "member_detached";
    case GalacticEventKind::formation_land: return "formation_land";
    case GalacticEventKind::fleet_slot_changed: return "fleet_slot_changed";
    case GalacticEventKind::movement_begin: return "movement_begin";
    case GalacticEventKind::movement_finished: return "movement_finished";
    case GalacticEventKind::movement_canceled: return "movement_canceled";
    case GalacticEventKind::movement_bounced: return "movement_bounced";
    case GalacticEventKind::movement_conflict: return "movement_conflict";
    case GalacticEventKind::land_conflict_requested: return "land_conflict_requested";
    case GalacticEventKind::combat_imminent: return "combat_imminent";
    case GalacticEventKind::planet_combat_begin: return "planet_combat_begin";
    case GalacticEventKind::conflict_pending: return "conflict_pending";
    case GalacticEventKind::child_battle_selected: return "child_battle_selected";
    case GalacticEventKind::load_fixup_completed: return "load_fixup_completed";
    case GalacticEventKind::planet_discovered: return "planet_discovered";
    case GalacticEventKind::intelligence_refreshed: return "intelligence_refreshed";
    case GalacticEventKind::discovery_feedback: return "discovery_feedback";
    case GalacticEventKind::production_started: return "production_started";
    case GalacticEventKind::production_completed: return "production_completed";
    case GalacticEventKind::production_cancelled: return "production_cancelled";
    case GalacticEventKind::auto_resolve_won: return "auto_resolve_won";
    case GalacticEventKind::auto_resolve_lost: return "auto_resolve_lost";
    case GalacticEventKind::victory_pending: return "victory_pending";
    case GalacticEventKind::victory_matured: return "victory_matured";
    case GalacticEventKind::victory_rejected: return "victory_rejected";
    case GalacticEventKind::auto_resolve_completed: return "auto_resolve_completed";
    }
    return "unknown";
}

std::string_view to_string(const GalacticRejectReason reason) noexcept {
    switch (reason) {
    case GalacticRejectReason::none: return "none";
    case GalacticRejectReason::unknown_subject: return "unknown_subject";
    case GalacticRejectReason::unknown_value: return "unknown_value";
    case GalacticRejectReason::not_authorized: return "not_authorized";
    case GalacticRejectReason::inactive_seat: return "inactive_seat";
    case GalacticRejectReason::paused: return "paused";
    case GalacticRejectReason::invalid_membership: return "invalid_membership";
    case GalacticRejectReason::incompatible_location: return "incompatible_location";
    case GalacticRejectReason::movement_ability_required: return "movement_ability_required";
    case GalacticRejectReason::resource_limit: return "resource_limit";
    case GalacticRejectReason::invalid_path: return "invalid_path";
    case GalacticRejectReason::cannot_depart: return "cannot_depart";
    case GalacticRejectReason::movement_blocked: return "movement_blocked";
    case GalacticRejectReason::not_paused: return "not_paused";
    case GalacticRejectReason::victory_unavailable: return "victory_unavailable";
    case GalacticRejectReason::observer_input: return "observer_input";
    case GalacticRejectReason::production_admission: return "production_admission";
    }
    return "unknown";
}

} // namespace eawr::sim::galactic
