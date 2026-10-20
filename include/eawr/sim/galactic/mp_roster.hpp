#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/galactic/types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// #3768: the multiplayer campaign roster every peer agrees on before a galactic session exists. It names
// the authored campaign and difficulty, each faction (data-driven names, any number of AI factions), every
// seat with its human side, the lead of each side, the agreed AI controller configuration per AI faction
// and the host's claim of the tick-zero digest. It carries no galaxy: each peer derives the galaxy, the
// content identities and its own tick-zero digest from local content (campaign/mp_agreement.hpp) and
// compares them with these claims. Header-only so the wire codec in net and the derivation in campaign
// share one definition without a library dependency.
namespace eawr::sim::galactic {

inline constexpr std::size_t mp_roster_max_ai_configuration = 4096;
inline constexpr std::size_t mp_roster_max_text = 256;

struct MpRosterPlayer {
    PlayerId player{};
    FactionId faction_id{};
    std::uint32_t flags{};
    std::string faction; // authored faction name; any faction the data declares, renamed or added
    bool active{true};
    FactionControl dropout{FactionControl::ai}; // WGS-P04: what a human faction becomes when its side empties
    std::vector<std::uint8_t> ai_configuration; // agreed AI controller configuration; empty for human factions
    friend bool operator==(const MpRosterPlayer&, const MpRosterPlayer&) = default;
};

struct MpRoster {
    std::string campaign, difficulty;
    bool custom{};
    std::int64_t credits{}, starting_tech{}, maximum_tech{};
    bool allow_heroes{true};
    std::vector<MpRosterPlayer> players; // strictly ascending player ID
    std::vector<Seat> seats;             // strictly ascending seat ID; humans first by convention, AI/script seats after
    std::vector<TeamLead> teams;         // strictly ascending human side ID
    std::array<std::uint8_t, 32> tick_zero{}; // the host's claim; a peer never adopts it
    friend bool operator==(const MpRoster&, const MpRoster&) = default;
};

namespace mp_roster_detail {
template<class T> core::Result<T> refuse(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(diagnostic_codes::invalid_setup);
    diagnostic.message = "multiplayer campaign roster: " + std::move(message);
    return core::Result<T>::failure(std::move(diagnostic));
}
} // namespace mp_roster_detail

// Human seats: up to eight sharing one faction against AI, or exactly two human sides, each side one faction.
// A third side, a duplicate or unlisted seat, a human seat on a faction another side holds, a lead outside its
// side and an AI configuration on a human faction are refused. The galaxy's own validate_setup runs again on
// the derived setup; these rules need no content. A host sealing its own roster passes require_claims = false
// before the AI configurations are filled in.
[[nodiscard]] inline core::Result<void> validate_mp_roster(const MpRoster& roster, bool require_claims = true) {
    using mp_roster_detail::refuse;
    const auto text_ok = [](std::string_view value) {
        return !value.empty() && value.size() <= mp_roster_max_text
            && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 0x20 && c != 0x7f; });
    };
    if (!text_ok(roster.campaign) || !text_ok(roster.difficulty))
        return refuse<void>("campaign and difficulty names are required plain text");
    if (roster.custom ? (roster.credits < 0 || roster.starting_tech < 0 || roster.maximum_tech < roster.starting_tech)
                      : (roster.credits != 0 || roster.starting_tech != 0 || roster.maximum_tech != 0 || !roster.allow_heroes))
        return refuse<void>("custom options are out of range or set without the custom flag");
    if (roster.players.size() < 2 || roster.players.size() > galactic_max_players)
        return refuse<void>("a campaign roster lists two to " + std::to_string(galactic_max_players) + " factions");
    std::set<FactionId> faction_ids;
    std::set<std::string> names;
    PlayerId previous{};
    for (const auto& player : roster.players) {
        if (player.player <= previous || player.faction_id == 0 || !text_ok(player.faction)
            || !faction_ids.insert(player.faction_id).second
            || !names.insert(player.faction).second
            || (player.dropout != FactionControl::ai && player.dropout != FactionControl::passive)
            || player.ai_configuration.size() > mp_roster_max_ai_configuration)
            return refuse<void>("factions need ascending player IDs, distinct nonzero faction IDs and names, "
                "a bounded AI configuration and an AI or passive dropout");
        previous = player.player;
    }
    const auto listed = [&](PlayerId id) {
        return std::any_of(roster.players.begin(), roster.players.end(), [&](const auto& row) { return row.player == id; });
    };
    if (roster.seats.empty() || roster.seats.size() > galactic_max_seats || roster.seats.front().seat_id.value == 0)
        return refuse<void>("seat IDs are nonzero and at most " + std::to_string(galactic_max_seats));
    std::set<HumanSideId> sides;
    std::size_t humans = 0;
    SeatId last{};
    for (const auto& seat : roster.seats) {
        if (!(last < seat.seat_id) || !listed(seat.player))
            return refuse<void>("seat IDs must be strictly ascending and name a listed faction");
        last = seat.seat_id;
        if (seat.kind == SeatKind::human) {
            if (seat.human_side_id == 0) return refuse<void>("a human seat needs a nonzero side");
            if (++humans > galactic_max_human_seats)
                return refuse<void>("at most " + std::to_string(galactic_max_human_seats) + " human seats");
            sides.insert(seat.human_side_id);
            if (sides.size() > 2) return refuse<void>("human seats share one side or form exactly two sides; a third side is refused");
        } else if (seat.kind == SeatKind::ai || seat.kind == SeatKind::script) {
            if (seat.human_side_id != 0) return refuse<void>("an AI or script seat has no human side");
        } else {
            return refuse<void>("unknown seat kind");
        }
    }
    if (humans < 2) return refuse<void>("multiplayer needs at least two human seats");
    if (roster.teams.size() != sides.size()) return refuse<void>("every human side needs exactly one lead entry");
    std::set<PlayerId> human_players;
    HumanSideId previous_side{};
    for (const auto& team : roster.teams) {
        if (team.side <= previous_side || !sides.contains(team.side))
            return refuse<void>("lead entries are ascending and name a human side");
        previous_side = team.side;
        PlayerId faction{};
        bool lead_found{};
        for (const auto& seat : roster.seats) if (seat.kind == SeatKind::human && seat.human_side_id == team.side) {
            if (faction != 0 && faction != seat.player) return refuse<void>("one human side holds one faction");
            faction = seat.player;
            lead_found |= seat.seat_id == team.lead && seat.active;
        }
        const bool any_active = std::any_of(roster.seats.begin(), roster.seats.end(), [&](const auto& seat) {
            return seat.kind == SeatKind::human && seat.human_side_id == team.side && seat.active; });
        if (!human_players.insert(faction).second)
            return refuse<void>("two human sides cannot hold the same faction");
        if (any_active ? !lead_found : team.lead.value != 0)
            return refuse<void>("a side's lead is an active seat of that side");
    }
    for (const auto& player : roster.players) {
        const bool human = human_players.contains(player.player);
        if ((human && !player.ai_configuration.empty()) || (require_claims && !human && player.ai_configuration.empty()))
            return refuse<void>("exactly the non-human factions carry an AI controller configuration");
        const bool active_seat = std::any_of(roster.seats.begin(), roster.seats.end(), [&](const auto& seat) {
            return seat.player == player.player && seat.active; });
        if (!player.active && active_seat) return refuse<void>("an active seat names an inactive faction");
    }
    return core::Result<void>::success();
}

} // namespace eawr::sim::galactic
