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

    // The colour follows the controlling seat: a gift moves the unit between the untinted own look and the mate's shade.
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

    // Eight humans on one side: the local seat (3, untinted) and seven mates are all distinct, in both modes.
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
            if (i == 2) ok = ok && !colour; // the local seat
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
    // Seven mates and a list of seven: every mate gets its own entry (local seat 3 untinted).
    std::array<ui::BattleRgb, 7> seven{};
    for (std::size_t i = 0; i < 7; ++i)
        seven[i] = {static_cast<std::uint8_t>(10 + 30 * i), 200, static_cast<std::uint8_t>(250 - 30 * i)};
    std::set<ui::BattleRgb> listed_seen;
    for (std::uint32_t i = 0; i < 8; ++i) {
        if (i == 2) continue;
        const auto colour = ui::gc_participant_colour({i + 1}, many_seats, many_teams, many_colours, true, {3},
            ui::BattleRgb{100, 0, 0}, {}, seven);
        ok = ok && colour.has_value() && *colour == seven[i < 2 ? i : i - 1];
        if (colour) listed_seen.insert(*colour);
    }
    ok = ok && listed_seen.size() == 7;
    if (!ok) std::cerr << "GC participant colours failed\n";
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
