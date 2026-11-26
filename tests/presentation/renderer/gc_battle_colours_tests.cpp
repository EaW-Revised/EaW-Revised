#include "eawr/presentation/ui/gc_battle_colours.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <span>

int main() {
    using namespace eawr;
    namespace ui = presentation::ui;
    using namespace sim::galactic;
    // Seats: 1 and 2 are two humans of player 1 on side 2 (seat 1 is the local seat), 3 is an enemy human on side 5,
    // 4 is an AI seat of player 3 that the caller reports as allied, 5 is an enemy AI seat.
    const std::array<Seat, 5> seats{{
        {{1}, 1, sim::SeatKind::human, 0, true, 2}, {{2}, 1, sim::SeatKind::human, 1, true, 2},
        {{3}, 2, sim::SeatKind::human, 2, true, 5}, {{4}, 3, sim::SeatKind::ai, 3, true, 0},
        {{5}, 4, sim::SeatKind::ai, 4, true, 0}}};
    const std::array<TeamLead, 2> teams{{{2, {1}, false}, {5, {3}, false}}};
    const std::array<ui::BattleRgb, 3> palette{{{100, 0, 0}, {0, 100, 0}, {0, 0, 100}}};
    const ui::BattleRgb faction{30, 40, 50};
    const std::array<sim::SeatId, 1> ai_mates{{{4}}};
    const auto participant = [&](unsigned seat, bool shades, unsigned local = 1) {
        return ui::gc_participant_colour(sim::SeatId{seat}, seats, teams, palette, shades, sim::SeatId{local}, faction, ai_mates);
    };
    const auto distance = [](const ui::BattleRgb& a, const ui::BattleRgb& b) {
        int total = 0;
        for (std::size_t i = 0; i < 3; ++i) total += std::abs(int{a[i]} - int{b[i]});
        return total;
    };
    const auto mate = participant(2, true);
    const auto ai_mate = participant(4, true);
    // Own units and every enemy (human or AI) keep the faction's own look: no entry.
    bool ok = !participant(1, true) && !participant(3, true) && !participant(5, true) && !participant(9, true)
        && !participant(1, false) && !participant(3, false);
    // A human mate gets a clear shade of the local faction colour; an allied AI mate another one.
    ok = ok && mate == ui::BattleRgb{97, 104, 111} && ai_mate.has_value() && mate != ai_mate
        && distance(*mate, faction) >= 150 && distance(*ai_mate, faction) >= 150 && distance(*mate, *ai_mate) >= 100;
    // Workshop switch: the mate takes a palette colour of its own.
    ok = ok && participant(2, false) == palette[0];
    // A palette entry equal to the local faction's colour is skipped for the mates' own colours.
    ok = ok && ui::gc_participant_colour({2}, seats, teams, palette, false, {1}, ui::BattleRgb{100, 0, 0}) == palette[1];
    // Seen from the enemy side (no allied AI reported): seat 3 is untinted and everyone else has no entry.
    ok = ok && !ui::gc_participant_colour({3}, seats, teams, palette, true, {3}, ui::BattleRgb{9, 9, 9})
        && !ui::gc_participant_colour({1}, seats, teams, palette, true, {3}, ui::BattleRgb{9, 9, 9})
        && !ui::gc_participant_colour({2}, seats, teams, palette, true, {3}, ui::BattleRgb{9, 9, 9});
    // A white faction still gets a distinct mate; without a local seat or faction nothing is overridden.
    ok = ok && ui::gc_participant_colour({2}, seats, teams, palette, true, {1}, ui::BattleRgb{255, 255, 255})
        != ui::BattleRgb{255, 255, 255}
        && !ui::gc_participant_colour({2}, seats, teams, palette, true) && !participant(2, true, 0);

    // Without a team colour (no Color in the data) the main seat has no entry: a gift moves the unit between the colour-off
    // look and the mate's shade.
    const std::map<sim::SeatId, ui::BattleRgb> live_colours{{sim::SeatId{2}, *mate}, {sim::SeatId{4}, *ai_mate}};
    sim::tactical::TacticalInstance gifted;
    gifted.owner = 1;
    gifted.controller_seat = {1};
    const auto before_b = ui::gc_instance_model_colour(false, gifted, faction, seats, live_colours);
    const auto before_c = ui::gc_instance_model_colour(true, gifted, faction, seats, live_colours);
    gifted.controller_seat = {2}; // Gift keeps owner 1 but changes the controlling seat.
    const auto after_b = ui::gc_instance_model_colour(false, gifted, faction, seats, live_colours);
    const auto after_c = ui::gc_instance_model_colour(true, gifted, faction, seats, live_colours);
    gifted.controller_seat = {3}; // An enemy controller keeps the faction look.
    const auto enemy_colour = ui::gc_instance_model_colour(true, gifted, faction, seats, live_colours);
    gifted.controller_seat = {}; // Rosterless / single-player fallback resolves the owner's first human seat: seat 1.
    const auto owner_colour = ui::gc_instance_model_colour(true, gifted, faction, seats, live_colours);
    gifted.owner = 3; // An allied AI's units have no human seat to resolve, so the AI seat stands in.
    const auto ai_colour = ui::gc_instance_model_colour(true, gifted, faction, seats, live_colours);
    ok = ok && before_b == faction && after_b == faction && before_c == faction && after_c == mate
        && before_c != after_c && enemy_colour == faction && owner_colour == faction && ai_colour == ai_mate
        && ui::gc_model_colour(false, faction, mate) == faction && ui::gc_model_colour(true, faction, mate) == mate
        && ui::gc_model_colour(true, faction, std::nullopt) == faction && !ui::gc_model_colour(false, {}, mate);

    // Eight humans on one side, seen from seat 3: the main seat 1 (#1 of the slot, untinted without a team colour) and seven
    // mates are all distinct, in both modes.
    std::array<Seat, 9> many_seats{};
    std::array<TeamLead, 1> many_teams{{{1, {1}, false}}};
    std::array<ui::BattleRgb, 9> many_colours{};
    for (std::uint32_t i = 0; i < 8; ++i) {
        many_seats[i] = {{i + 1}, 1, sim::SeatKind::human, i, true, 1};
        many_colours[i] = {static_cast<std::uint8_t>(40 * i), static_cast<std::uint8_t>(255 - 30 * i), 60};
    }
    many_seats[8] = {{9}, 9, sim::SeatKind::ai, 8, true, 0};
    for (const bool shades : {true, false}) {
        std::set<ui::BattleRgb> seen;
        for (std::uint32_t i = 0; i < 8; ++i) {
            const auto colour = ui::gc_participant_colour({i + 1}, many_seats, many_teams, many_colours, shades,
                {3}, ui::BattleRgb{100, 0, 0});
            if (i == 0) ok = ok && !colour; // the main seat
            else if (colour) seen.insert(*colour);
        }
        ok = ok && seen.size() == 7;
        for (const auto& colour : seen) ok = ok && colour != ui::BattleRgb{100, 0, 0};
    }
    ok = ok && !ui::gc_participant_colour({9}, many_seats, many_teams, many_colours, true, {3}, ui::BattleRgb{100, 0, 0});
    // PROVISIONAL (legacy #8359): the faction's Coop_Player_Colours list. Entry n is the n-th mate (humans first, then
    // allied AI); a shorter list falls back to shades past its end; no list is the default; the local seat and enemies
    // stay untinted.
    const std::array<ui::BattleRgb, 2> listed{{{0, 128, 128}, {200, 10, 10}}};
    const auto with_list = [&](unsigned seat, std::span<const ui::BattleRgb> list, bool shades = true) {
        return ui::gc_participant_colour(sim::SeatId{seat}, seats, teams, palette, shades, sim::SeatId{1}, faction, ai_mates, list);
    };
    ok = ok && with_list(2, listed) == listed[0] && with_list(4, listed) == listed[1] // human mate 1st, allied AI 2nd
        && with_list(2, listed, false) == listed[0]                                   // beats the workshop palette switch
        && !with_list(1, listed) && !with_list(3, listed) && !with_list(5, listed);
    const std::span<const ui::BattleRgb> one{listed.data(), 1};
    ok = ok && with_list(2, one) == listed[0] && with_list(4, one) == ai_mate;        // shorter list: shade past its end
    ok = ok && with_list(2, {}) == mate && with_list(4, {}) == ai_mate;               // absent list = today's default
    // Seven mates and a list of seven: every mate gets its own entry in slot order (main seat 1 untinted).
    std::array<ui::BattleRgb, 7> seven{};
    for (std::size_t i = 0; i < 7; ++i)
        seven[i] = {static_cast<std::uint8_t>(10 + 30 * i), 200, static_cast<std::uint8_t>(250 - 30 * i)};
    std::set<ui::BattleRgb> listed_seen;
    for (std::uint32_t i = 0; i < 8; ++i) {
        if (i == 0) continue;
        const auto colour = ui::gc_participant_colour({i + 1}, many_seats, many_teams, many_colours, true, {3},
            ui::BattleRgb{100, 0, 0}, {}, seven);
        ok = ok && colour.has_value() && *colour == seven[i - 1];
        if (colour) listed_seen.insert(*colour);
    }
    ok = ok && listed_seen.size() == 7;

    // Owner correction (legacy #8359, 11-25): the main player (local seat) takes the faction's COLOURED main team colour
    // (Factions.xml Color, e.g. Rebel red); mates keep their shades / list entries; enemies keep the colour-off look.
    const ui::BattleRgb team{200, 30, 30};
    const auto coloured = [&](unsigned seat, std::span<const ui::BattleRgb> list = {}, bool shades = true) {
        return ui::gc_participant_colour(sim::SeatId{seat}, seats, teams, palette, shades, sim::SeatId{1}, faction,
            ai_mates, list, team);
    };
    ok = ok && coloured(1) == team && coloured(1, {}, false) == team && coloured(1, listed) == team  // main
        && coloured(2) == mate && coloured(4) == ai_mate                                             // mates 1..n as before
        && coloured(2, listed) == listed[0] && coloured(4, listed) == listed[1]
        && coloured(2, {}, false) == palette[0]
        && !coloured(3) && !coloured(5) && !coloured(9);                                             // enemies untinted
    // Seen from the enemy seat 3, the team colour argument is that seat's own faction: seat 3 is main, the rest stay out.
    ok = ok && ui::gc_participant_colour({3}, seats, teams, palette, true, {3}, ui::BattleRgb{9, 9, 9}, {}, {}, team) == team
        && !ui::gc_participant_colour({1}, seats, teams, palette, true, {3}, ui::BattleRgb{9, 9, 9}, {}, {}, team);
    // No colour-off faction colour (own_colour off): nothing is overridden, the team colour included.
    ok = ok && !ui::gc_participant_colour({1}, seats, teams, palette, true, {1}, {}, ai_mates, {}, team);
    // Gifts re-tint by the same rule: a unit given to the main seat takes the team colour, one given to a mate its shade.
    const std::map<sim::SeatId, ui::BattleRgb> main_colours{
        {sim::SeatId{1}, team}, {sim::SeatId{2}, *mate}, {sim::SeatId{4}, *ai_mate}};
    sim::tactical::TacticalInstance given;
    given.owner = 1;
    given.controller_seat = {2};
    const auto at_mate = ui::gc_instance_model_colour(true, given, faction, seats, main_colours);
    given.controller_seat = {1}; // mate gives it to the main player
    const auto at_main = ui::gc_instance_model_colour(true, given, faction, seats, main_colours);
    given.controller_seat = {}; // no controller recorded: the owner's first human seat (the main seat 1)
    const auto at_owner = ui::gc_instance_model_colour(true, given, faction, seats, main_colours);
    given.controller_seat = {3}; // given to an enemy seat: colour-off look
    const auto at_enemy = ui::gc_instance_model_colour(true, given, faction, seats, main_colours);
    ok = ok && at_mate == mate && at_main == team && at_owner == team && at_enemy == faction
        && ui::gc_instance_model_colour(false, given, faction, seats, main_colours) == faction;
    // Eight humans seen from seat 3: the main seat 1 is the team colour, the seven mates distinct shades, none equal to it.
    std::set<ui::BattleRgb> main_seen;
    for (std::uint32_t i = 0; i < 8; ++i) {
        const auto colour = ui::gc_participant_colour({i + 1}, many_seats, many_teams, many_colours, true, {3},
            ui::BattleRgb{100, 0, 0}, {}, {}, team);
        ok = ok && colour.has_value();
        if (i == 0) ok = ok && colour == team;
        if (colour) main_seen.insert(*colour);
    }
    ok = ok && main_seen.size() == 8;

    // GCC-08 (owner decision 11-25, legacy #8359): the main player is #1 of the FACTION's lobby slot, never the host and
    // never the local seat, the same on every peer. Seat 1 is the host's seat but sits at lobby position #2 (lobby_slot 1);
    // seat 2, the guest, is #1 (lobby_slot 0). Seat 6 is an enemy human.
    std::array<Seat, 3> slot_seats{{
        {{1}, 1, sim::SeatKind::human, 1, true, 1}, {{2}, 1, sim::SeatKind::human, 0, true, 1},
        {{6}, 2, sim::SeatKind::human, 2, true, 2}}};
    const std::array<TeamLead, 2> slot_teams{{{1, {1}, false}, {2, {6}, false}}};
    const auto view = [&](std::span<const Seat> table, unsigned local, std::span<const ui::BattleRgb> list = {}) {
        std::map<sim::SeatId, ui::BattleRgb> out;
        for (const auto& row : table)
            if (const auto colour = ui::gc_participant_colour(row.seat_id, table, slot_teams, palette, true,
                    sim::SeatId{local}, faction, {}, list, team)) out.emplace(row.seat_id, *colour);
        return out;
    };
    const auto host_view = view(slot_seats, 1);
    const auto guest_view = view(slot_seats, 2);
    ok = ok && ui::gc_main_seat(slot_seats, 1) == sim::SeatId{2} && ui::gc_main_seat(slot_seats, 2) == sim::SeatId{6}
        && host_view == guest_view && host_view.size() == 2 && host_view.at({2}) == team    // the guest (#1) is main
        && host_view.at({1}) == ui::detail::gc_shade(faction, 1) && !host_view.contains({6}); // the host (#2) a shade
    // A unit with no recorded controller belongs to the faction's #1; a gift re-tints it by the same table.
    sim::tactical::TacticalInstance unit;
    unit.owner = 1;
    const auto unit_colour = [&](const std::map<sim::SeatId, ui::BattleRgb>& table, std::span<const Seat> rows,
                                 unsigned seat) {
        unit.controller_seat = {seat};
        return ui::gc_instance_model_colour(true, unit, faction, rows, table);
    };
    ok = ok && unit_colour(host_view, slot_seats, 0) == team && unit_colour(host_view, slot_seats, 1) == host_view.at({1})
        && unit_colour(host_view, slot_seats, 2) == team;
    // #1 leaves mid-game (its seat turns inactive in the shared seat table): #2, the host, becomes main on every peer, the
    // seat that left has no entry, and units given to or owned by the new #1 take the team colour.
    auto after_leave = slot_seats;
    after_leave[1].active = false;
    const auto host_after = view(after_leave, 1);
    ok = ok && ui::gc_main_seat(after_leave, 1) == sim::SeatId{1} && host_after.size() == 1 && host_after.at({1}) == team
        && unit_colour(host_after, after_leave, 1) == team && unit_colour(host_after, after_leave, 0) == team
        && unit_colour(host_after, after_leave, 2) == faction; // still on the leaver's seat: colour-off look
    // Three players: #1 leaves, #2 becomes main and #3 moves up to the 1st mate's colour (here the list entry).
    std::array<Seat, 4> three{{
        {{1}, 1, sim::SeatKind::human, 2, true, 1}, {{2}, 1, sim::SeatKind::human, 0, true, 1},
        {{3}, 1, sim::SeatKind::human, 1, true, 1}, {{6}, 2, sim::SeatKind::human, 3, true, 2}}};
    const auto before3 = view(three, 1, listed);
    ok = ok && before3 == view(three, 3, listed) && before3.at({2}) == team && before3.at({3}) == listed[0]
        && before3.at({1}) == listed[1];
    three[1].active = false;
    const auto after3 = view(three, 1, listed);
    ok = ok && after3 == view(three, 3, listed) && !after3.contains({2}) && after3.at({3}) == team
        && after3.at({1}) == listed[0];
    // A co-op seat admitted mid-campaign shares the faction's lobby slot and joins behind it (seat ID breaks the tie).
    const std::array<Seat, 2> admitted{{{{7}, 1, sim::SeatKind::human, 4, true, 1}, {{5}, 1, sim::SeatKind::human, 4, true, 1}}};
    ok = ok && ui::gc_main_seat(admitted, 1) == sim::SeatId{5} && ui::gc_main_seat(admitted, 3).value == 0;
    // Models require two active human seats of the local faction; minimap's existing colour lookup does not.
    auto solo = seats;
    solo[1].active = false;
    sim::tactical::TacticalInstance subject;
    subject.owner = 1;
    subject.controller_seat = {1};
    ok = ok && ui::gc_coop_model_colour(true, subject, faction, solo, main_colours, 1) == faction
        && ui::gc_instance_model_colour(true, subject, faction, solo, main_colours) == team
        && ui::gc_coop_model_colour(false, subject, faction, seats, main_colours, 1) == faction
        && ui::gc_coop_model_colour(true, subject, faction, seats, main_colours, 1) == team;
    // A spectator follows view player 1, independent of their unrelated seat 3. Following player 2 (one human) is off.
    const auto spectator_view = ui::gc_main_seat(seats, 1);
    ok = ok && spectator_view == sim::SeatId{1}
        && ui::gc_participant_colour({1}, seats, teams, palette, true, spectator_view, faction, ai_mates, {}, team) == team
        && ui::gc_coop_model_colour(true, subject, faction, seats, main_colours, 1) == team
        && ui::gc_coop_model_colour(true, subject, faction, seats, main_colours, 2) == faction;
    subject.controller_seat = {2};
    ok = ok && ui::gc_coop_model_colour(true, subject, faction, seats, main_colours, 1) == mate
        && ui::gc_instance_model_colour(true, subject, faction, seats, main_colours) == mate;
    subject.owner = 3;
    subject.controller_seat = {4};
    ok = ok && ui::gc_coop_model_colour(true, subject, faction, solo, main_colours, 1) == faction
        && ui::gc_instance_model_colour(true, subject, faction, solo, main_colours) == ai_mate;
    if (!ok) std::cerr << "GC participant colours failed\n";
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
