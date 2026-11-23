#include "eawr/presentation/ui/gc_battle_colours.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

int main() {
    using namespace eawr;
    namespace ui = presentation::ui;
    using namespace sim::galactic;
    const std::array<Seat, 4> seats{{
        {{1}, 1, sim::SeatKind::human, 0, true, 2}, {{2}, 1, sim::SeatKind::human, 1, true, 2},
        {{3}, 2, sim::SeatKind::human, 2, true, 5}, {{4}, 3, sim::SeatKind::ai, 3, true, 0}}};
    const std::array<TeamLead, 2> teams{{{2, {1}, false}, {5, {3}, false}}};
    const std::array<ui::BattleRgb, 3> palette{{{100, 0, 0}, {0, 100, 0}, {0, 0, 100}}};
    const auto participant = [&](unsigned seat, bool shades) {
        return ui::gc_participant_colour(sim::SeatId{seat}, seats, teams, palette, shades);
    };
    const ui::BattleRgb faction{30, 40, 50};
    const auto before = participant(1, true);
    const auto after = participant(2, true);
    const std::map<sim::SeatId, ui::BattleRgb> live_colours{{sim::SeatId{1}, *before}, {sim::SeatId{2}, *after}};
    sim::tactical::TacticalInstance gifted;
    gifted.owner = 1;
    gifted.controller_seat = {1};
    const auto before_b = ui::gc_instance_model_colour(false, gifted, faction, seats, live_colours);
    const auto before_c = ui::gc_instance_model_colour(true, gifted, faction, seats, live_colours);
    gifted.controller_seat = {2}; // Gift keeps owner 1 but changes the controlling seat.
    const auto after_b = ui::gc_instance_model_colour(false, gifted, faction, seats, live_colours);
    const auto after_c = ui::gc_instance_model_colour(true, gifted, faction, seats, live_colours);
    gifted.controller_seat = {4}; // An AI controller always keeps the faction colour.
    const auto ai_colour = ui::gc_instance_model_colour(true, gifted, faction, seats, live_colours);
    gifted.controller_seat = {}; // Rosterless / single-player fallback resolves the owner.
    const auto owner_colour = ui::gc_instance_model_colour(true, gifted, faction, seats, live_colours);
    bool ok = before == palette[0] && participant(3, true) == palette[1]
        && after == ui::BattleRgb{117, 28, 28} && participant(2, false) == palette[2]
        && !participant(4, true) && !participant(9, false)
        && ui::gc_model_colour(false, faction, before) == faction
        && ui::gc_model_colour(false, faction, after) == faction
        && ui::gc_model_colour(true, faction, before) != ui::gc_model_colour(true, faction, after)
        && ui::gc_model_colour(true, faction, participant(4, true)) == faction
        && !ui::gc_model_colour(false, {}, before)
        && before_b == faction && after_b == faction && before_c == before && after_c == after
        && before_c != after_c && ai_colour == faction && owner_colour == before;
    std::array<Seat, 9> many_seats{};
    std::array<TeamLead, 8> many_teams{};
    std::array<ui::BattleRgb, 9> many_colours{};
    for (std::uint32_t i = 0; i < 8; ++i) {
        many_seats[i] = {{i + 1}, i + 1, sim::SeatKind::human, i, true, i + 1};
        many_teams[i] = {i + 1, {i + 1}, false};
        many_colours[i] = {static_cast<std::uint8_t>(i), 0, 0};
    }
    many_seats[8] = {{9}, 9, sim::SeatKind::ai, 8, true, 0};
    for (std::uint32_t i = 0; i < 8; ++i)
        ok = ok && ui::gc_participant_colour({i + 1}, many_seats, many_teams, many_colours, true) == many_colours[i];
    ok = ok && !ui::gc_participant_colour({9}, many_seats, many_teams, many_colours, true);
    for (std::uint32_t i = 0; i < 8; ++i) many_seats[i].human_side_id = 1;
    for (std::uint32_t i = 1; i < 8; ++i)
        ok = ok && ui::gc_participant_colour({i}, many_seats, std::span(many_teams).first(1), palette, true)
            != ui::gc_participant_colour({i + 1}, many_seats, std::span(many_teams).first(1), palette, true);
    if (!ok) std::cerr << "GC participant colours failed\n";
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
