#include "galactic_support.hpp"
#include "galactic/weapon_action_live_fixture.hpp"
#include "galactic/hud_clock_fixture.hpp"
#include "galactic/galaxy_view.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/platform/sim_workers.hpp"

// The live acceptance fixture behind the viewer's "hack-live" HUD case: the host, not a copied snapshot, owns the
// menu. Choose queues the exact typed capability; cancel retracts it; neither moves anything else.
namespace {
namespace gc = eawr::sim::galactic;
namespace view = eawr::viewer::galactic;
using galactic_test::expect;
template<class T> T take(eawr::core::Result<T> r) {
    if (!r) throw std::runtime_error(r.error().message);
    return std::move(r).value();
}
void take(eawr::core::Result<void> r) { if (!r) throw std::runtime_error(r.error().message); }

constexpr gc::PlayerId local = 1;
gc::GalacticSetup make_setup(eawr::sim::SeatId& seat) {
    auto setup = view::synthetic_live_setup();
    view::configure_synthetic_hud_clock(setup);
    std::vector<gc::FleetDisplayType> traits;
    std::vector<gc::FleetDisplayVisibility> visibility;
    std::map<gc::TypeId, std::string> names;
    view::configure_weapon_action_live_fixture(setup, local, traits, visibility, names);
    for (const auto& row : setup.seats) if (row.player == local && row.active && row.kind == eawr::sim::SeatKind::human) { seat = row.seat_id; break; }
    return setup;
}
eawr::sim::math::Fixed credits(const eawr::campaign::GameSession& game) {
    for (const auto& row : game.galaxy().snapshot()->player_economy()->players) if (row.player == local) return row.credits;
    throw std::runtime_error("local player has no account");
}
gc::PlayerId owner(const eawr::campaign::GameSession& game, gc::PlanetId planet) {
    for (const auto& row : game.galaxy().snapshot()->planets()) if (row.id == planet) return row.owner;
    throw std::runtime_error("unknown planet");
}
bool present(const eawr::campaign::GameSession& game, eawr::sim::EntityId id) {
    const auto records = game.galaxy().snapshot()->actor_records();
    return std::any_of(records.begin(), records.end(), [&](const auto& row) { return row.id == id; });
}
void order(eawr::campaign::GameSession& game, eawr::sim::SeatId seat, std::uint64_t sequence, gc::CommandPayload payload) {
    take(game.submit({{game.completed_tick(), seat, sequence}, std::move(payload)}));
}
gc::PreparedAbility open_menu(eawr::campaign::GameSession& game, eawr::sim::SeatId seat,
    const eawr::sim::PartitionExecutor& pool) {
    take(game.step(pool));
    order(game, seat, 1, gc::PrepareGalacticAbility{view::live_hack_actor, view::live_hack_planet, gc::AbilityKind::superweapon_hack});
    take(game.step(pool));
    const auto* menus = game.galaxy().snapshot()->hero_action_menus();
    expect(menus && menus->player == local && menus->hack && menus->hack->live_prepared && !menus->theft,
        "the real seat command publishes a live hack menu for the local player");
    const auto& hack = *menus->hack;
    expect(hack.prepared.actor == view::live_hack_actor && hack.prepared.planet == view::live_hack_planet
        && hack.prepared.prepared_price == eawr::sim::math::Fixed::from_integer(250).value() && hack.affordable
        && hack.weapon == 7009 && hack.weapon_type == view::live_hack_required_type,
        "the menu carries the authored price, the display-only weapon and an affordable verdict");
    return *hack.live_prepared;
}
} // namespace

int main() {
    try {
        const eawr::platform::ThreadWorkerAdapter pool(1);
        eawr::sim::SeatId seat{};
        {   // Choose: the typed capability executes the hack through the shared host.
            const auto setup = make_setup(seat);
            expect(seat.value != 0, "the fixture has an active human seat for the local player");
            auto game = take(eawr::campaign::GameSession::create(setup));
            const auto before = [&] { take(game.step(pool)); return credits(game); }();
            const auto owner_before = owner(game, view::live_hack_planet);
            const auto prepared = open_menu(game, seat, pool);
            order(game, seat, 2, gc::ExecutePreparedAbility{prepared});
            take(game.step(pool));
            expect(credits(game) == take(eawr::sim::math::subtract(before, eawr::sim::math::Fixed::from_integer(250).value())),
                "the local human route pays the prepared price once");
            expect(owner(game, view::live_hack_planet) == view::synthetic_neutral_owner
                && owner(game, view::live_hack_planet) != owner_before, "the planet is forced to the Neutral player");
            expect(!present(game, view::live_hack_actor) && !present(game, 7009),
                "the hacker and the orbital fleets leave through canonical loss");
            expect(game.galaxy().ability_state()->results.size() == 1
                && game.galaxy().ability_state()->results[0].outcome == gc::AbilityOutcome::purchased
                && game.galaxy().ability_state()->results[0].kind == gc::AbilityKind::superweapon_hack, "typed purchased result");
        }
        {   // Cancel: retracts the retained preparation and changes nothing else.
            const auto setup = make_setup(seat);
            auto game = take(eawr::campaign::GameSession::create(setup));
            take(game.step(pool));
            const auto before = credits(game);
            const auto owner_before = owner(game, view::live_hack_planet);
            const auto prepared = open_menu(game, seat, pool);
            order(game, seat, 2, gc::CancelGalacticAbility{prepared.actor, prepared.preparation});
            take(game.step(pool));
            expect(credits(game) == before && owner(game, view::live_hack_planet) == owner_before
                && present(game, view::live_hack_actor) && present(game, 7009), "cancel moves nothing");
            const auto* menus = game.galaxy().snapshot()->hero_action_menus();
            expect(!menus || !menus->hack, "the retracted menu is no longer published");
            expect(game.galaxy().ability_state()->results.size() == 1
                && game.galaxy().ability_state()->results[0].outcome == gc::AbilityOutcome::cancelled, "typed cancelled result");
        }
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
