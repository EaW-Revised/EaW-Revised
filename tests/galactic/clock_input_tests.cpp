#include "galactic/clock_input.hpp"
#include "../presentation/galactic/hud_clock_fixture.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>

int main() {
    using namespace eawr;
    using Reason = sim::galactic::GalacticRejectReason;
    viewer::galactic::PauseToggleGuard guard;
    std::array<sim::galactic::GalacticEvent, 1> events{};
    auto& rejected = events.front();
    rejected.kind = sim::galactic::GalacticEventKind::command_rejected;
    rejected.producer = sim::SeatId{1};
    // Two mouse/key toggles before publication reserve only one nested pause.
    assert(guard.begin(false, true));
    assert(!guard.begin(false, true));
    rejected.reason = Reason::unknown_subject; // invalid move from the same seat
    guard.observe(false, sim::SeatId{1}, events);
    assert(!guard.begin(false, true));
    rejected.reason = Reason::not_paused; // belongs to resume, not pending pause
    guard.observe(false, sim::SeatId{1}, events);
    assert(!guard.begin(false, true));
    rejected.reason = Reason::inactive_seat;
    rejected.producer = sim::SeatId{2};
    guard.observe(false, sim::SeatId{1}, events);
    assert(!guard.begin(false, true));
    guard.observe(true, sim::SeatId{1}, {}); // accepted pause publication
    assert(guard.begin(true, false));
    assert(!guard.begin(true, false));
    rejected.producer = sim::SeatId{1};
    rejected.reason = Reason::production_admission;
    guard.observe(true, sim::SeatId{1}, events);
    assert(!guard.begin(true, false));
    rejected.reason = Reason::not_paused;
    guard.observe(true, sim::SeatId{1}, events);
    assert(guard.begin(true, false)); // resume rejection releases its reservation
    guard.observe(false, sim::SeatId{1}, {});
    assert(guard.begin(false, true));
    rejected.reason = Reason::inactive_seat;
    guard.observe(false, sim::SeatId{1}, events);
    assert(guard.begin(false, true));
    sim::galactic::GalacticSetup fixture;
    sim::galactic::GalacticPlayer player;
    player.player_id = 7;
    fixture.players.push_back(player);
    sim::galactic::Planet planet;
    planet.id = 42;
    fixture.planets.push_back(planet);
    viewer::galactic::configure_synthetic_hud_clock(fixture);
    assert(fixture.rules.fiscal_cycle_frames == 300);
    assert(fixture.player_economy && fixture.fiscal);
    assert(fixture.player_economy->players.front().player == 7);
    const auto scale = sim::math::Fixed::scale;
    assert(fixture.player_economy->players.front().credits.raw() == 12422 * scale + scale / 2);
    assert(fixture.fiscal->planets.front().planet == 42);
    assert(fixture.fiscal->planets.front().credits.raw() == 0);
    assert(!fixture.fiscal->players.front().earns_planet_income);
}
