#include "galactic_support.hpp"

#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The galactic simulation skeleton (ADR-012): setup validation, the debug commands, the sectioned
// state hash against a from-scratch hash, worker-count and storage-order independence, the
// off-thread hasher and the galactic-gather phase.
namespace {

using namespace galactic_test;
using galactic::GalacticSession;
using galactic::GalacticSetup;

class FailingExecutor final : public eawr::sim::PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 1; }
    [[nodiscard]] eawr::core::Result<void> execute(std::size_t, const std::function<void(std::size_t)>&) const override {
        eawr::core::Diagnostic diagnostic;
        diagnostic.code = "EAWR-SIM-0109";
        diagnostic.message = "injected worker failure";
        return eawr::core::Result<void>::failure(diagnostic);
    }
};

struct Trace {
    std::vector<std::string> hashes;
    std::vector<std::string> snapshots;
    std::vector<galactic::GalacticTick::Work> work;
};

[[nodiscard]] Trace run(const GalacticSetup& setup, const eawr::sim::PartitionExecutor& executor, const bool scramble,
    std::shared_ptr<eawr::sim::StateHasher> hasher = {}, const bool check_scratch = false) {
    Trace trace;
    auto created = GalacticSession::create(setup);
    expect(static_cast<bool>(created), "synthetic galaxy creates a session");
    if (!created) return trace;
    auto session = std::move(created).value();
    session.set_state_hasher(std::move(hasher));
    auto commands = script(setup);
    // Submission order across seats does not matter; per seat it follows (tick, sequence).
    std::stable_sort(commands.begin(), commands.end(), [](const galactic::GameCommand& left, const galactic::GameCommand& right) {
        return left.key.seat > right.key.seat;
    });
    for (const auto& command : commands) expect(static_cast<bool>(session.submit(command)), "scripted command admitted");
    for (std::uint64_t tick = 0; tick < 45; ++tick) {
        if (scramble) session.scramble_storage_for_testing();
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "galactic step succeeds");
        if (!stepped) break;
        const auto& result = stepped.value();
        const auto hash = result.state_hash.get();
        if (check_scratch) {
            expect(hash == session.state_sha256(), "tick " + std::to_string(result.completed_tick)
                    + ": the sectioned hash equals a from-scratch hash");
        }
        trace.hashes.push_back(hash);
        trace.snapshots.push_back(result.snapshot->sha256());
        trace.work.push_back(result.work);
    }
    return trace;
}

void setup_validation() {
    const auto good = galactic::synthetic_galaxy();
    expect(static_cast<bool>(galactic::validate_setup(good)), "the synthetic galaxy is valid");
    expect(good == galactic::synthetic_galaxy(), "the synthetic galaxy is deterministic");
    galactic::SyntheticGalaxyOptions coop;
    coop.coop_seat = true;
    const auto shared = galactic::synthetic_galaxy(coop);
    expect(static_cast<bool>(galactic::validate_setup(shared)) && shared.seats.size() == 3 && shared.seats[2].player == 1,
        "the coop galaxy seats two humans in player 1");

    const auto refused = [&](const std::string_view name, const std::function<void(GalacticSetup&)>& change) {
        auto setup = good;
        change(setup);
        expect(has_code(galactic::validate_setup(setup), galactic::diagnostic_codes::invalid_setup),
            std::string(name) + " is refused with EAWR-SIM-0501");
        expect(!GalacticSession::create(setup), std::string(name) + ": no session");
    };
    refused("no players", [](GalacticSetup& setup) { setup.players.clear(); });
    refused("player zero", [](GalacticSetup& setup) { setup.players.front().player_id = 0; });
    refused("unsorted players", [](GalacticSetup& setup) { std::swap(setup.players[0], setup.players[1]); });
    refused("seat zero", [](GalacticSetup& setup) { setup.seats.front().seat_id = {0}; });
    refused("unsorted seats", [](GalacticSetup& setup) { std::swap(setup.seats[0], setup.seats[1]); });
    refused("seat in no player", [](GalacticSetup& setup) { setup.seats.front().player = 99; });
    refused("unknown seat kind", [](GalacticSetup& setup) { setup.seats.front().kind = static_cast<galactic::SeatKind>(9); });
    refused("human without a side", [](GalacticSetup& setup) { setup.seats.front().human_side_id = 0; });
    refused("system issuer with a human side", [](GalacticSetup& setup) { setup.seats.front().kind = galactic::SeatKind::ai; });
    refused("nine human seats", [](GalacticSetup& setup) {
        setup.seats.clear();
        for (std::uint32_t id = 1; id <= 9; ++id) setup.seats.push_back({{id}, 1, galactic::SeatKind::human, id - 1, true, 1});
    });
    refused("nine inactive human seats", [](GalacticSetup& setup) {
        setup.seats.clear();
        for (std::uint32_t id = 1; id <= 9; ++id) setup.seats.push_back({{id}, 1, galactic::SeatKind::human, id - 1, false, 1});
    });
    refused("three human sides", [](GalacticSetup& setup) {
        setup.seats.push_back({{3}, 1, galactic::SeatKind::human, 2, true, 3});
    });
    struct HumanShape { std::uint32_t count; std::uint32_t first_side; };
    for (const auto shape : {HumanShape{8, 8}, HumanShape{8, 4}, HumanShape{4, 1}}) {
        auto setup = good;
        setup.seats.clear();
        for (std::uint32_t id = 1; id <= shape.count; ++id) {
            // Ownership alternates independently of the human-side split.
            setup.seats.push_back({{id}, id % 2 + 1, galactic::SeatKind::human, id - 1, true,
                id <= shape.first_side ? 1U : 2U});
        }
        // Additional AI/script sides and uncontrolled players do not count as human sides.
        for (std::uint32_t player = 3; player <= 64; ++player) setup.players.push_back({player, player, 0});
        for (std::uint32_t player = 3; player < 64; ++player) {
            const auto kind = player % 2 == 0 ? galactic::SeatKind::ai : galactic::SeatKind::script;
            setup.seats.push_back({{player + shape.count - 2}, player, kind, player + shape.count - 3, true, 0});
        }
        expect(static_cast<bool>(galactic::validate_setup(setup)) && static_cast<bool>(GalacticSession::create(setup)),
            "coop 8v-AI / versus 4v4 / versus 1v3 with independent owners and AI/script sides are valid");
        for (auto& seat : setup.seats) { seat.kind = galactic::SeatKind::ai; seat.human_side_id = 0; }
        expect(static_cast<bool>(galactic::validate_setup(setup)), "AI-only setup does not consume human slots");
    }
    auto regrouped = good;
    regrouped.seats[0].human_side_id = 42;
    expect(GalacticSession::create(good).value().state_sha256() != GalacticSession::create(regrouped).value().state_sha256(),
        "explicit human-side membership is hashed independently of ownership");
    refused("unsorted planets", [](GalacticSetup& setup) { std::swap(setup.planets[0], setup.planets[1]); });
    refused("planet ID zero", [](GalacticSetup& setup) { setup.planets.front().id = 0; });
    refused("unknown planet owner", [](GalacticSetup& setup) { setup.planets.front().owner = 99; });
    refused("route to an unknown planet", [](GalacticSetup& setup) { setup.routes.front().to = 9999; });
    refused("route to itself", [](GalacticSetup& setup) { setup.routes.front().to = setup.routes.front().from; });
    refused("route ID taken by a planet", [](GalacticSetup& setup) { setup.routes.front().id = setup.planets.back().id; });
    refused("fleet orbiting nothing", [](GalacticSetup& setup) { setup.fleets.front().orbit = 9999; });
    refused("unknown fleet owner", [](GalacticSetup& setup) { setup.fleets.front().owner = 99; });
    refused("empty fleet", [](GalacticSetup& setup) { setup.fleets.front().members.clear(); });
    refused("exhausted entity allocator", [](GalacticSetup& setup) {
        setup.fleets.back().members.back().id = std::numeric_limits<eawr::sim::EntityId>::max();
    });
    refused("member ID taken", [](GalacticSetup& setup) { setup.fleets.back().members.front().id = setup.fleets.front().id; });
    refused("duplicate member", [](GalacticSetup& setup) {
        setup.fleets.back().members.front().id = setup.fleets.front().members.front().id;
    });
}

void submission_rules() {
    namespace codes = galactic::diagnostic_codes;
    auto setup = galactic::synthetic_galaxy();
    const auto planet = setup.planets[0].id;
    {
        // A real (or multiplayer) session never admits the debug commands.
        setup.debug_commands = false;
        auto session = GalacticSession::create(setup).value();
        expect(has_code(session.submit(owner(0, {1}, 1, planet, 2)), codes::invalid_command),
            "debug set-owner is refused without the setup flag");
        expect(has_code(session.submit(orbit(0, {1}, 1, setup.fleets[0].id, planet)), codes::invalid_command),
            "debug set-orbit is refused without the setup flag");
        expect(session.pending_command_count() == 0, "nothing queued");
        setup.debug_commands = true;
    }
    auto session = GalacticSession::create(setup).value();
    const eawr::sim::InlineExecutor inline_executor;
    expect(has_code(session.submit(owner(0, {9}, 1, planet, 2)), codes::unknown_issuer), "an unknown seat is refused");
    expect(has_code(session.submit({{0, {1}, 1}, galactic::TacticalLayerCommand{7, 1, {}}}), codes::invalid_command),
        "a tactical-layer command is refused by the galaxy");
    expect(has_code(session.submit(owner(galactic::galactic_max_ticks, {1}, 1, planet, 2)), codes::resource_limit),
        "a tick past the limit is refused");
    expect(static_cast<bool>(session.submit(owner(2, {1}, 5, planet, 2))), "a command is admitted");
    expect(has_code(session.submit(owner(2, {1}, 5, planet, 1)), codes::command_order), "the same (tick, sequence) again is refused");
    expect(has_code(session.submit(owner(1, {1}, 9, planet, 1)), codes::command_order),
        "an earlier tick than the seat's previous command is refused");
    expect(static_cast<bool>(session.submit(owner(1, {2}, 1, planet, 1))), "another seat's earlier tick is admitted");
    const auto before = session.state_sha256();
    expect(session.pending_command_count() == 2, "refusals queue nothing");
    for (int index = 0; index < 3; ++index) static_cast<void>(session.step(inline_executor));
    expect(has_code(session.submit(owner(2, {2}, 7, planet, 1)), codes::late_command), "a command for an executed tick is refused");
    expect(session.state_sha256() != before, "the admitted commands changed the planet");
    expect(session.executed_commands().size() == 2 && session.executed_commands()[0].key.seat == eawr::sim::SeatId{2}
            && session.executed_commands()[1].key.tick == 2,
        "commands execute in canonical (tick, seat, sequence) order");
}

void execution_and_events() {
    const auto setup = galactic::synthetic_galaxy();
    auto session = GalacticSession::create(setup).value();
    const eawr::sim::InlineExecutor executor;
    const auto planet = setup.planets[0];
    const auto fleet = setup.fleets[0];
    const auto other = setup.planets[1].id;
    expect(fleet.owner == 1, "the first fleet is player 1's");
    expect(session.snapshot()->completed_tick() == 0 && session.snapshot()->planets().size() == setup.planets.size(),
        "the tick-zero snapshot shows the setup");
    expect(static_cast<bool>(session.submit(owner(0, {1}, 1, planet.id, 2))), "owner change");
    expect(static_cast<bool>(session.submit(orbit(0, {1}, 2, fleet.id, other))), "orbit change");
    expect(static_cast<bool>(session.submit(orbit(0, {1}, 3, planet.id, other))), "wrong subject");
    expect(static_cast<bool>(session.submit(owner(0, {1}, 4, planet.id, 77))), "unknown player");
    expect(static_cast<bool>(session.submit(orbit(0, {2}, 1, fleet.id, planet.id))), "another player's fleet");
    expect(static_cast<bool>(session.submit(owner(1, {1}, 5, setup.planets[2].id, 99))), "rejected later");
    const auto first = session.step(executor).value();
    const auto events = first.snapshot->events();
    using Kind = galactic::GalacticEventKind;
    using Reason = galactic::GalacticRejectReason;
    expect(events.size() == 7, "seven events in tick 0");
    if (events.size() == 7) {
        expect(events[0].kind == Kind::command_executed && events[1].kind == Kind::planet_owner_changed
                && events[1].subject == planet.id && events[1].value == 2 && events[1].producer == eawr::sim::SeatId{1},
            "the owner change follows its command, produced by its seat");
        expect(events[2].kind == Kind::command_executed && events[3].kind == Kind::fleet_orbit_changed && events[3].value == other,
            "the orbit change follows its command");
        expect(events[4].kind == Kind::command_rejected && events[4].reason == Reason::unknown_subject,
            "a fleet command on a planet is rejected at execution");
        expect(events[5].kind == Kind::command_rejected && events[5].reason == Reason::unknown_value,
            "an unknown player is rejected at execution");
        expect(events[6].kind == Kind::command_rejected && events[6].reason == Reason::not_authorized && events[6].producer == eawr::sim::SeatId{2},
            "a seat of another player may not order the fleet (ADR-020)");
    }
    expect(first.snapshot->planets()[0].owner == 2 && first.snapshot->fleets()[0].orbit == other,
        "the snapshot shows the committed state");
    // The owner change is a scoring event (WGP-101), so the campaign-scoring section exists from this tick.
    expect(first.work.sections_encoded == 6 && first.work.sections_reused == 0, "the first tick encodes every section");
    const auto second = session.step(executor).value();
    expect(second.snapshot->events().size() == 1 && second.snapshot->events()[0].kind == Kind::command_rejected,
        "a rejected command changes nothing");
    expect(second.work.sections_encoded == 1 && second.work.sections_reused == 5,
        "a tick that changes nothing re-encodes only the session section");
    expect(second.state_sha256 == session.state_sha256(), "and still equals a from-scratch hash");
    expect(session.executed_commands().size() == 6, "every command is recorded as executed");

    // A failing phase fails the step after its commands ran; the session refuses to continue.
    expect(static_cast<bool>(session.submit(owner(2, {1}, 6, planet.id, 1))), "queued");
    const FailingExecutor failing;
    const auto failed = session.step(failing);
    expect(!failed && failed.error().code == galactic::diagnostic_codes::worker_failure, "a failed phase fails the step");
    expect(session.completed_tick() == 2, "a failed step completes no tick");
    const auto again = session.step(executor);
    expect(!again && again.error().code == galactic::diagnostic_codes::worker_failure,
        "after a failed phase the session refuses every further step");
}

void coop_last_order_wins() {
    galactic::SyntheticGalaxyOptions options;
    options.coop_seat = true;
    const auto setup = galactic::synthetic_galaxy(options);
    auto session = GalacticSession::create(setup).value();
    const eawr::sim::InlineExecutor executor;
    const auto fleet = setup.fleets[0];
    const auto coop = setup.seats[2].seat_id;
    // Both seats of player 1 order the same fleet in one tick, submitted in the "wrong" order:
    // both apply in canonical seat order, so the higher seat's order wins (ADR-020).
    expect(static_cast<bool>(session.submit(orbit(0, coop, 1, fleet.id, setup.planets[5].id))), "coop seat order");
    expect(static_cast<bool>(session.submit(orbit(0, {1}, 1, fleet.id, setup.planets[4].id))), "first seat order");
    const auto stepped = session.step(executor).value();
    expect(stepped.snapshot->fleets()[0].orbit == setup.planets[5].id, "the later seat's order wins in one tick");
    // Across ticks the later tick wins, whichever seat gave it.
    expect(static_cast<bool>(session.submit(orbit(1, coop, 2, fleet.id, setup.planets[6].id))), "coop seat again");
    expect(static_cast<bool>(session.submit(orbit(2, {1}, 2, fleet.id, setup.planets[7].id))), "first seat later");
    static_cast<void>(session.step(executor));
    static_cast<void>(session.step(executor));
    expect(session.snapshot()->fleets()[0].orbit == setup.planets[7].id, "the later tick wins");
}

void determinism() {
    galactic::SyntheticGalaxyOptions options;
    options.planets = 120; // 120 planets, 150 routes, 9 fleets: over the single-partition cutoff
    options.players = 3;
    options.fleets_per_player = 3;
    const auto setup = galactic::synthetic_galaxy(options);
    const eawr::sim::InlineExecutor inline_executor;
    const auto reference = run(setup, inline_executor, false, {}, true);
    expect(reference.hashes.size() == 45, "the reference run completes");
    expect(std::adjacent_find(reference.hashes.begin(), reference.hashes.end()) == reference.hashes.end(),
        "every tick has its own hash (the session section carries the tick)");
    bool reused = false;
    for (const auto& work : reference.work) reused = reused || work.sections_reused >= 3;
    expect(reused, "quiet ticks reuse clean section digests");
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        for (const bool scramble : {false, true}) {
            const auto trace = run(setup, pool, scramble, {}, scramble);
            const auto label = std::to_string(workers) + " workers" + (scramble ? ", scrambled storage" : "");
            expect(trace.hashes == reference.hashes, label + ": identical state hashes");
            expect(trace.snapshots == reference.snapshots, label + ": identical snapshots");
        }
    }
    const eawr::platform::ThreadWorkerAdapter pool(4);
    const auto hashed = run(setup, pool, true, std::make_shared<eawr::platform::ThreadStateHasher>());
    expect(hashed.hashes == reference.hashes, "the off-thread hasher gives the synchronous hashes");
}

void gather_phase() {
    const eawr::platform::ThreadWorkerAdapter pool(4);
    for (const auto planets : {std::uint32_t{12}, std::uint32_t{120}}) {
        galactic::SyntheticGalaxyOptions options;
        options.planets = planets;
        auto session = GalacticSession::create(galactic::synthetic_galaxy(options)).value();
        const PhaseProbe probe(pool);
        const auto stepped = session.step(probe);
        expect(static_cast<bool>(stepped), "probed step succeeds");
        const auto large = stepped && stepped.value().work.gathered_objects >= 256;
        expect(probe.phases.size() == 1 && probe.phases[0].first == "galactic-gather"
                && probe.phases[0].second == (large ? std::size_t{64} : std::size_t{1}),
            "galactic-gather reaches the executor by name, with 64 partitions from 256 objects and one below ("
                + std::to_string(planets) + " planets)");
    }
}

} // namespace

int main() {
    setup_validation();
    submission_rules();
    execution_and_events();
    coop_last_order_wins();
    determinism();
    gather_phase();
    if (failures != 0) {
        std::cerr << failures << " galactic session check(s) failed\n";
        return 1;
    }
    std::cout << "galactic session contracts passed\n";
    return 0;
}
