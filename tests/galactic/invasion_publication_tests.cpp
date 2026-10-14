#include "galactic_support.hpp"
#include "eawr/campaign/invasion_offer.hpp"
#include "eawr/campaign/tactical_return.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <cstdlib>
#include <new>
#include <stdexcept>

namespace {
thread_local bool count_allocations{};
thread_local std::size_t allocations{};
}
#if defined(_MSC_VER)
#define EAWR_TEST_NOINLINE __declspec(noinline)
#else
#define EAWR_TEST_NOINLINE __attribute__((noinline))
#endif
EAWR_TEST_NOINLINE void* operator new(std::size_t size) {
    if (count_allocations) ++allocations;
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc{};
}
EAWR_TEST_NOINLINE void operator delete(void* memory) noexcept { std::free(memory); }
EAWR_TEST_NOINLINE void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

#undef EAWR_TEST_NOINLINE

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::campaign;
using galactic_test::expect;
using eawr::sim::math::Fixed;
template<class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
    return std::move(result).value();
}
Fixed f(std::int64_t value) { return Fixed::from_integer(value).value(); }
gc::GalacticSetup fixture() {
    gc::GalacticSetup setup;
    setup.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    setup.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}, {{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    setup.planets = {{1, {}, 2}, {2, {f(12), {}, {}}, 2}};
    setup.fleets = {{10, 1, 1, {{20, 2}}}}; setup.debug_commands = true;
    setup.rules.maximum_fleet_movement_distance = f(12); setup.rules.fiscal_cycle_frames = 100;
    setup.fleet_control.emplace(); setup.fleet_control->types = {{1, false, true, false}, {2, true, true, false}};
    setup.movement.emplace(); auto& movement = *setup.movement;
    movement.mode_modifier = f(1);
    movement.types = {{1, f(1), f(100), 0, true}, {2, f(1), f(50), 0, true, false, false, false, true}};
    movement.players = {{1, f(1), f(1), false}, {2, f(1), f(1), false}, {3, f(1), f(1), false}};
    movement.planets = {{1, f(1), false}, {2, f(1), false}};
    movement.contents = {{20, {{30, 1}}}};
    for (const auto& player : setup.players) for (const auto& planet : setup.planets)
        movement.access.push_back({player.player_id, planet.id, true, false});
    setup.control.emplace(); auto& control = *setup.control;
    control.change_seconds = f(7); control.max_ground_forces = 10;
    control.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& player : control.players) for (const auto& other : control.players)
        control.relations.push_back({player.player, other.player, player.player == other.player});
    control.types = {{1, 1, false, false}, {2, 0, false, true}};
    for (const auto& planet : setup.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 100 + planet.id; row.surface_accessible = true;
        control.planets.push_back(row);
    }
    return setup;
}
void publication_contract(unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto world = take(gc::GalacticSession::create(fixture()));
    cp::ConflictRecord context; context.id = 40; context.planet = 1; context.invader = 1; context.defender = 2;
    cp::BattleResult result; result.conflict = 40; result.winner = 1;
    result.invasion_offers = cp::tactical_invasion_offers(context, result, *world.snapshot());
    cp::InvasionOfferPublication publication;
    const auto original = publication.publish(&result, &context, *world.snapshot(), 1, true, 0);
    expect(original && original->candidates.size() == 1 && original->cargo.size() == 1,
        "publication fixture gathers an actual loaded transport");
    const auto* candidate_storage = original->candidates.data();
    const auto* cargo_storage = original->cargo.data();
    const auto generation = world.snapshot()->observer_input_generation();
    (void)take(world.step(executor));
    expect(world.snapshot()->observer_input_generation() == generation, "quiet strategic step retains input generation");
    allocations = 0; count_allocations = true;
    const auto quiet = publication.publish(&result, &context, *world.snapshot(), 1, true, 0);
    count_allocations = false;
    expect(allocations == 0 && publication.gathers() == 1 && quiet == original
        && quiet->candidates.data() == candidate_storage && quiet->cargo.data() == cargo_storage,
        "quiet step makes zero publication allocations/gathers and retains vector storage");
    (void)take(world.submit({{world.completed_tick(), {1}, 1}, gc::PauseGalaxy{}}));
    (void)take(world.step(executor));
    const auto paused = publication.publish(&result, &context, *world.snapshot(), 1, true, 0);
    expect(paused != quiet && !paused->accept_permitted, "pause change refreshes acceptance permission");
    const auto paused_gathers = publication.gathers();
    (void)take(world.step(executor));
    allocations = 0; count_allocations = true;
    const auto paused_quiet = publication.publish(&result, &context, *world.snapshot(), 1, true, 0);
    count_allocations = false;
    expect(allocations == 0 && paused_quiet == paused && publication.gathers() == paused_gathers,
        "unchanged paused step also has zero allocations/gathers and retains publication storage");
    (void)take(world.submit({{world.completed_tick(), {1}, 2}, gc::ResumeGalaxy{}}));
    (void)take(world.step(executor));
    const auto resumed = publication.publish(&result, &context, *world.snapshot(), 1, true, 0);
    expect(resumed != paused && resumed->accept_permitted, "resume refreshes the real pause permission");
    const auto unavailable = publication.publish(&result, &context, *world.snapshot(), 1, false, 0);
    expect(unavailable != resumed && !unavailable->accept_permitted,
        "real native availability change refreshes publication permission");
    result.invasion_offers.front().transports.clear();
    const auto consumed = publication.publish(&result, &context, *world.snapshot(), 1, true, 1);
    expect(consumed != unavailable && consumed->candidates.empty(),
        "selection revision refreshes consumed retained identities");
    result.invasion_offers = {{1, 10, 1, {20}}};
    const auto prepared_again = publication.publish(&result, &context, *world.snapshot(), 1, true, 2);
    expect(prepared_again->candidates.size() == 1, "restored selection revision gathers before an ownership change");
    auto submitted = world.submit(galactic_test::owner(world.completed_tick(), {1}, 3, 1, 1));
    if (!submitted) throw std::runtime_error(eawr::core::format_diagnostic(submitted.error()));
    (void)take(world.step(executor));
    const auto own = publication.publish(&result, &context, *world.snapshot(), 1, true, 2);
    expect(own->candidates.empty() && own != prepared_again, "ownership alone refreshes and suppresses the offer");
    auto cargo_world = take(gc::GalacticSession::create(fixture()));
    const auto loaded = publication.publish(&result, &context, *cargo_world.snapshot(), 1, true, 2);
    const std::array<eawr::sim::EntityId, 1> destroyed{30};
    (void)take(cargo_world.apply_battle_losses(destroyed));
    const auto empty = publication.publish(&result, &context, *cargo_world.snapshot(), 1, true, 2);
    expect(loaded->cargo.size() == 1 && empty->cargo.empty() && empty->candidates.empty() && loaded != empty,
        "actual direct cargo loss refreshes the publication without a result revision change");
}
}
int main() {
    try { for (const unsigned workers : {1U, 2U, 4U, 8U}) publication_contract(workers); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
