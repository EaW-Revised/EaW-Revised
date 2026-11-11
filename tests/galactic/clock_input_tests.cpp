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

    // #5355: one LAN strategic speed request in flight per seat, so a double press cannot spend the 2-per-10 s budget.
    {
        using Kind = sim::galactic::GalacticEventKind;
        constexpr std::int64_t one = std::int64_t{1} << 24, two = 2 * one;
        const sim::SeatId local{1};
        viewer::galactic::SpeedToggleGuard speed;
        std::array<sim::galactic::GalacticEvent, 1> answer{};
        auto& event = answer.front();
        assert(!speed.begin(one, one, 10)); // the current speed is not a request
        assert(speed.begin(one, two, 10) && speed.target() == two);
        assert(!speed.begin(one, two, 11) && !speed.begin(one, one, 11)); // a second press waits, either direction
        speed.observe(one, local, {}, 12);
        assert(speed.target() == two); // the agreed snapshot has not answered yet
        event.kind = Kind::speed_changed; event.producer = local; event.subject = 0U; event.tick = 9;
        speed.observe(one, local, answer, 13);
        assert(speed.target() == two); // an older speed event of this seat belongs to an earlier request
        event.producer = sim::SeatId{2}; event.tick = 14;
        speed.observe(one, local, answer, 14);
        assert(speed.target() == two); // another seat's change does not answer this seat
        event.kind = Kind::command_rejected; event.producer = local; event.reason = sim::galactic::GalacticRejectReason::unknown_subject;
        speed.observe(one, local, answer, 14);
        assert(speed.target() == two); // an unrelated refusal of this seat does not release it
        speed.observe(two, local, {}, 15);
        assert(!speed.target()); // the agreed 2x answers the request
        assert(speed.begin(two, one, 15) && !speed.begin(two, one, 16)); // back to 1x: one request again
        event.reason = sim::galactic::GalacticRejectReason::not_authorized; event.tick = 18;
        speed.observe(two, local, answer, 18);
        assert(!speed.target()); // a sim refusal of this seat releases it
        assert(speed.begin(two, one, 20));
        event.kind = Kind::speed_changed; event.tick = 22;
        speed.observe(two, local, answer, 22);
        assert(!speed.target()); // this seat's own speed event releases it even if another change followed
        assert(speed.begin(two, one, 30));
        speed.reset(); // the adapter's budget refusal for this seat
        assert(!speed.target() && speed.begin(two, one, 31));
        speed.observe(two, local, {}, 31 + viewer::galactic::SpeedToggleGuard::release_ticks - 1);
        assert(speed.target() == one);
        speed.observe(two, local, {}, 31 + viewer::galactic::SpeedToggleGuard::release_ticks);
        assert(!speed.target()); // never stuck: one speed window of session ticks releases it
    }
}
