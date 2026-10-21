#include "../galactic/galactic_support.hpp"
#include "../galactic/raid_fixture.hpp"
#include "galactic_lan_match.hpp"
#include "eawr/platform/live_game_lockstep.hpp"
#include "eawr/net/galactic_adapter.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "eawr/sim/galactic/fleet_intelligence.hpp"

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/synthetic.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/platform/tactical_feed.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "../../apps/viewer/src/galactic/live_battle_dialog_view.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

// GCMP-03 (#4359): two LiveGameSessions with their own simulation threads, joined by the real LockstepSession through
// GalacticLanMatch (the galaxy adapter and the desync monitor included). Local orders are untimed seat inputs, ticks run
// only on the agreed frontier (also while paused), and both peers keep equal per-tick digests at 1/2/4/8 workers.
namespace {

using galactic_test::expect;
namespace net = eawr::net;
namespace galactic = eawr::sim::galactic;
namespace math = eawr::sim::math;
namespace viewer = eawr::presentation::godot_backend;
using eawr::platform::LiveGameOrder;
using eawr::platform::LiveGameSession;
using namespace std::chrono_literals;

template <class T> T require(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}

galactic::GalacticSetup versus_setup(const std::uint32_t players = 3) {
    eawr::campaign::SyntheticCampaign campaign;
    campaign.name = "net-galactic-live-versus";
    campaign.galaxy.seed = 31;
    campaign.galaxy.planets = 24;
    campaign.galaxy.players = players;
    campaign.galaxy.fleets_per_player = 2;
    campaign.galaxy.members_per_fleet = 3;
    campaign.galaxy.debug_commands = true;
    campaign.fiscal_cycle_seconds = math::Fixed::from_decimal("2.5").value();
    return require(eawr::campaign::build_synthetic_campaign(campaign));
}
// The recorded seat-control policy (WGS-P01..P04): a leaving human is replaced by its AI fallback seat.
galactic::GalacticSetup policy_setup() {
    auto setup = versus_setup();
    setup.seats.push_back({{4}, 1, eawr::sim::SeatKind::ai, 0, false, 0});
    setup.seats.push_back({{5}, 2, eawr::sim::SeatKind::ai, 0, false, 0});
    setup.seat_control.emplace();
    setup.seat_control->teams.push_back({1, {1}, false});
    setup.seat_control->teams.push_back({2, {2}, false});
    for (std::uint32_t player = 1; player <= 3; ++player)
        setup.seat_control->controllers.push_back({player,
            player <= 2 ? galactic::FactionControl::human : galactic::FactionControl::ai, galactic::FactionControl::ai, {}, {}});
    return setup;
}
std::vector<eawr::sim::SeatId> human_seats(const galactic::GalacticSetup& setup) {
    std::vector<eawr::sim::SeatId> seats;
    for (const auto& seat : setup.seats) if (seat.kind == eawr::sim::SeatKind::human) seats.push_back(seat.seat_id);
    std::sort(seats.begin(), seats.end());
    return seats;
}

template <class Predicate> bool until(const Predicate& predicate, const std::chrono::milliseconds limit = 30s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// Thread-safe in-memory streams: each live session polls its own end from its simulation thread.
class Pipes {
public:
    explicit Pipes(const std::uint32_t count) : inbox_(count), open_(count, true) {}
    std::mutex mutex;
    std::vector<std::deque<net::Received>> inbox_;
    std::vector<bool> open_;
};
class PipeTransport final : public net::MessageTransport {
public:
    PipeTransport(Pipes& pipes, const net::PeerIndex self) : pipes_(&pipes), self_(self) {}
    eawr::core::Result<void> send(const net::PeerIndex to, const std::span<const std::uint8_t> frame) override {
        const std::lock_guard lock(pipes_->mutex);
        if (!pipes_->open_[to]) {
            eawr::core::Diagnostic error;
            error.code = "EAWR-TEST-PIPE-CLOSED";
            error.message = "pipe closed";
            return eawr::core::Result<void>::failure(std::move(error));
        }
        pipes_->inbox_[to].push_back({self_, {frame.begin(), frame.end()}});
        return eawr::core::Result<void>::success();
    }
    std::optional<net::Received> poll() override {
        const std::lock_guard lock(pipes_->mutex);
        auto& inbox = pipes_->inbox_[self_];
        if (inbox.empty()) return std::nullopt;
        auto received = std::move(inbox.front());
        inbox.pop_front();
        return received;
    }
    net::LinkState link(const net::PeerIndex peer) const override {
        const std::lock_guard lock(pipes_->mutex);
        return pipes_->open_[peer] ? net::LinkState::open : net::LinkState::closed;
    }
    net::PeerIndex self() const override { return self_; }
    std::uint32_t peer_count() const override { return static_cast<std::uint32_t>(pipes_->inbox_.size()); }
private:
    Pipes* pipes_;
    net::PeerIndex self_;
};

// Two peers over one setup. Transports and matches are declared first: they must outlive the live sessions.
struct Pair {
    Pair(galactic::GalacticSetup initial, const std::array<std::size_t, 2> workers,
        const LiveGameSession::Pacing pacing = LiveGameSession::Pacing::driven,
        const std::uint64_t notice_ms = 3000, const std::size_t started = 2,
        const std::uint64_t final_tick_count = net::max_session_ticks,
        const net::GalacticClockPolicy clock_policy = net::GalacticClockPolicy::two_per_45_seconds)
        : setup(std::move(initial)), humans(human_seats(setup)), pipes(2), worker_counts(workers), pacing_(pacing) {
        if (humans.size() != 2) throw std::runtime_error("the fixture needs exactly two human seats");
        viewer::GalacticLanOptions options;
        options.clock_policy = clock_policy;
        options.seats = {{humans[0], true}, {humans[1], true}};
        options.exchange_every = 10;
        options.waiting_notice_ms = notice_ms;
        options.final_tick_count = final_tick_count;
        for (net::PeerIndex peer = 0; peer < 2; ++peer) {
            transports[peer] = std::make_unique<PipeTransport>(pipes, peer);
            matches[peer] = std::make_unique<viewer::GalacticLanMatch>(*transports[peer], options);
        }
        for (net::PeerIndex peer = 0; peer < started; ++peer) start(peer);
    }
    ~Pair() {
        for (auto& live : lives) if (live) live->stop();
    }
    void start(const net::PeerIndex peer, const std::function<void(LiveGameSession::Options&)>& tweak = {}) {
        LiveGameSession::Options options;
        options.workers = worker_counts[peer];
        options.pacing = pacing_;
        options.step_driver = matches[peer]->driver();
        if (tweak) tweak(options);
        lives[peer] = require(LiveGameSession::start(setup, std::move(options)));
    }
    void order(const net::PeerIndex peer, galactic::CommandPayload payload) {
        lives[peer]->submit(LiveGameOrder{humans[peer], std::move(payload), {}});
    }
    // Driven pacing: both peers target `tick` and must reach it.
    bool run_to(const std::uint64_t tick) {
        for (auto& live : lives) live->advance_to(tick);
        bool reached = true;
        for (auto& live : lives) reached = live->wait_for(tick, 30s) && reached;
        return reached && !lives[0]->failure() && !lives[1]->failure();
    }
    [[nodiscard]] math::Fixed speed(const net::PeerIndex peer) const {
        return lives[peer]->frame().latest->clock().strategic_speed;
    }
    [[nodiscard]] std::uint32_t pause(const net::PeerIndex peer) const {
        return static_cast<std::uint32_t>(lives[peer]->frame().latest->clock().pause_depth);
    }
    [[nodiscard]] std::uint64_t galaxy_frame(const net::PeerIndex peer) const {
        return static_cast<std::uint64_t>(lives[peer]->frame().latest->clock().galactic_frame);
    }

    galactic::GalacticSetup setup;
    std::vector<eawr::sim::SeatId> humans;
    Pipes pipes;
    std::array<std::size_t, 2> worker_counts;
    LiveGameSession::Pacing pacing_;
    std::array<std::unique_ptr<PipeTransport>, 2> transports;
    std::array<std::unique_ptr<viewer::GalacticLanMatch>, 2> matches;
    std::array<std::unique_ptr<LiveGameSession>, 2> lives;
};

// The no-transport oracle: the same stamped commands, including the agreed LAN clock history.
std::vector<std::string> reference(const galactic::GalacticSetup& setup, std::vector<galactic::GameCommand> commands,
    const std::uint64_t ticks) {
    std::sort(commands.begin(), commands.end(), [](const auto& left, const auto& right) { return left.key < right.key; });
    auto game = require(eawr::campaign::GameSession::create(setup));
    const eawr::platform::ThreadWorkerAdapter executor(1);
    net::GalacticAdapterOptions options;
    options.clock_policy = net::GalacticClockPolicy::two_per_45_seconds;
    net::GalacticSessionAdapter adapter(game, executor, options);
    std::vector<std::string> hashes;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        std::vector<net::SeatInput> inputs;
        for (const auto& command : commands) if (command.key.tick == tick) {
            const eawr::sim::SeatCommandKey key{tick, command.key.seat, command.key.sequence};
            inputs.push_back({key, require(net::encode_galactic_input(key, command.payload))});
        }
        require(adapter.install(inputs));
        hashes.push_back(require(adapter.step()).state_sha256);
    }
    return hashes;
}

const math::Fixed double_speed = math::Fixed::from_raw(2 * math::Fixed::scale);

void agreed_commands_and_digests(const std::array<std::size_t, 2> workers) {
    const std::string label = "workers " + std::to_string(workers[0]) + "/" + std::to_string(workers[1]) + ": ";
    Pair pair(versus_setup(), workers);
    const auto planets = pair.setup.planets.size();
    expect(pair.run_to(20), label + "both peers reach tick 20 on the agreed frontier");
    // Orders from both seats, and a pause that only the agreed stream can deliver to both.
    pair.order(0, galactic::DebugSetPlanetOwner{pair.setup.planets[3 % planets].id, 1});
    pair.order(1, galactic::DebugSetPlanetOwner{pair.setup.planets[7 % planets].id, 2});
    pair.order(0, galactic::PauseGalaxy{});
    pair.order(1, galactic::DebugSetFleetOrbit{pair.setup.fleets[1].id, pair.setup.planets[11 % planets].id});
    expect(pair.run_to(40), label + "ticks keep running while the galaxy is paused");
    expect(pair.pause(0) > 0 && pair.pause(1) > 0, label + "the agreed pause reached both peers");
    for (const auto& match : pair.matches)
        expect(match->view().clock_actor == pair.humans[0] && match->view().clock_action == "paused" && match->view().clock_paused,
            label + "the HUD pause receipt names the actual human issuer");
    const auto frozen = pair.galaxy_frame(0);
    expect(pair.run_to(50), label + "the paused session still ticks");
    expect(pair.galaxy_frame(0) == frozen && pair.galaxy_frame(1) == frozen, label + "the paused galaxy frame does not move");
    pair.order(0, galactic::ResumeGalaxy{});
    pair.order(1, galactic::SetSpeed{false, double_speed});
    expect(pair.run_to(80), label + "resume and speed change are agreed commands");
    expect(pair.pause(0) == 0 && pair.pause(1) == 0, label + "the agreed resume reached both peers");
    for (const auto& match : pair.matches) expect(!match->view().clock_paused, label + "HUD observes the agreed resume");
    expect(pair.speed(0) == double_speed && pair.speed(1) == double_speed, label + "the agreed speed change reached both peers");
    expect(pair.galaxy_frame(0) > frozen && pair.galaxy_frame(0) == pair.galaxy_frame(1), label + "the galaxy runs again, equally");
    for (auto& live : pair.lives) live->stop();
    expect(!pair.lives[0]->failure() && !pair.lives[1]->failure(), label + "no failure");
    expect(pair.lives[0]->rejected_orders().empty() && pair.lives[1]->rejected_orders().empty(), label + "no refused order");
    const auto first = pair.lives[0]->tick_hashes();
    const auto second = pair.lives[1]->tick_hashes();
    expect(first.size() == 80 && second.size() == 80, label + "each peer completed exactly the demanded ticks");
    expect(first == second, label + "equal per-tick digests on both peers");
    // The executed stream, replayed without any network, reproduces the digests: nothing was stamped or stepped outside it.
    expect(reference(pair.setup, pair.lives[0]->record().commands, first.size()) == first, label + "the unnetworked replay agrees");
    const auto commands = pair.lives[0]->record().commands;
    const auto peer_commands = pair.lives[1]->record().commands;
    bool same = commands.size() == peer_commands.size() && !commands.empty();
    for (std::size_t index = 0; same && index < commands.size(); ++index)
        same = !(commands[index].key < peer_commands[index].key) && !(peer_commands[index].key < commands[index].key)
            && commands[index].payload == peer_commands[index].payload;
    expect(same, label + "both peers executed the same commands in the same order");
}

void raid_commands_and_digests() {
    for (const bool refused : {false, true}) {
        auto setup = raid_test::setup();
        if (refused) setup.movement->raid->rules.max_limited = 0;
        Pair pair(setup, {1, 2});
        pair.order(0, galactic::MoveFleet{10, {1, 2}, {}, true});
        expect(pair.run_to(32), "raid and refused-raid peers reach the agreed frontier");
        for (auto& live : pair.lives) live->stop();
        const auto first = pair.lives[0]->tick_hashes(), second = pair.lives[1]->tick_hashes();
        expect(first.size() == 32 && first == second, "raid/refusal holds equal per-tick digests on both peers");
        for (const auto& live : pair.lives) {
            expect(!live->failure() && live->rejected_orders().empty(), "raid input travels through the ordinary agreed inbox");
            const auto snapshot = live->frame().latest;
            const auto& fleet = snapshot->fleets().front();
            expect(fleet.raid_destination == (refused ? std::optional<galactic::PlanetId>{} : std::optional<galactic::PlanetId>{2}),
                "only an accepted raid records its destination on each peer");
            expect(fleet.orbit == (refused ? 1U : 2U), "deterministic raid qualification controls actual arrival");
            const auto commands = live->record().commands;
            expect(commands.size() == 1 && std::get<galactic::MoveFleet>(commands.front().payload).raid,
                "both accepted and refused raid orders are recorded with the canonical raid bit");
        }
        expect(reference(pair.setup, pair.lives[0]->record().commands, first.size()) == first,
            "raid/refusal command stream reproduces without the network");
    }
}

void real_time_follows_the_agreed_clock() {
    Pair pair(versus_setup(), {2, 2}, LiveGameSession::Pacing::real_time);
    pair.order(1, galactic::SetSpeed{false, double_speed}); // A separate seat budget from pause/resume.
    pair.order(1, galactic::DebugSetPlanetOwner{pair.setup.planets[2].id, 2});
    const bool reached = until([&] { return pair.lives[0]->completed_tick() >= 90 && pair.lives[1]->completed_tick() >= 90; });
    expect(reached, "real-time lockstep peers run on the shared cadence");
    pair.order(0, galactic::PauseGalaxy{});
    const auto paused = until([&] { return pair.pause(0) > 0 && pair.pause(1) > 0; });
    expect(paused, "a pause is agreed while real-time ticks keep flowing");
    const auto tick = pair.lives[0]->completed_tick();
    expect(until([&] { return pair.lives[0]->completed_tick() > tick + 6 && pair.lives[1]->completed_tick() > tick + 6; }),
        "a paused real-time lockstep session keeps its cadence");
    pair.order(0, galactic::ResumeGalaxy{});
    expect(until([&] { return pair.pause(0) == 0 && pair.pause(1) == 0; }), "the agreed resume runs while paused");
    for (auto& live : pair.lives) live->stop();
    expect(!pair.lives[0]->failure() && !pair.lives[1]->failure(), "real-time run has no failure");
    const auto first = pair.lives[0]->tick_hashes();
    const auto second = pair.lives[1]->tick_hashes();
    const auto common = std::min(first.size(), second.size());
    expect(common >= 90 && std::equal(first.begin(), first.begin() + static_cast<std::ptrdiff_t>(common), second.begin()),
        "real-time peers hold equal digests");
    expect(pair.speed(0) == double_speed && pair.speed(1) == double_speed, "the speed set by one seat applies to both");
}

void refusals_never_stamp_locally() {
    Pair pair(versus_setup(), {1, 1});
    expect(pair.run_to(5), "refusal fixture runs");
    auto& live = *pair.lives[0];
    live.submit(LiveGameOrder{pair.humans[0], galactic::PauseGalaxy{}, std::uint64_t{50}}); // timed
    live.submit(LiveGameOrder{pair.humans[1], galactic::PauseGalaxy{}, {}});                // another seat's order
    live.submit(galactic::GameCommand{{6, pair.humans[0], 1}, galactic::PauseGalaxy{}});        // pre-stamped command
    expect(until([&] { return live.rejected_orders().size() >= 3; }), "every local-stamp path is refused");
    for (const auto& error : live.rejected_orders()) expect(error.code == "EAWR-PLATFORM-0600", "refusal code");
    expect(pair.run_to(30), "refusals do not disturb the match");
    expect(pair.pause(0) == 0 && pair.pause(1) == 0, "none of the refused pauses executed");
    expect(pair.lives[1]->rejected_orders().empty(), "the other peer saw no refusal");
}

void views_connect_wait_and_run() {
    Pair pair(versus_setup(), {2, 2}, LiveGameSession::Pacing::driven, 40, 1);
    pair.lives[0]->advance_to(10);
    expect(until([&] { return pair.matches[0]->view().phase == viewer::GalacticLanPhase::connecting; }, 5s),
        "alone, the first peer is connecting");
    std::this_thread::sleep_for(100ms);
    expect(pair.lives[0]->completed_tick() == 0 && pair.matches[0]->view().phase == viewer::GalacticLanPhase::connecting,
        "no tick runs without the other peer");
    expect(!viewer::galactic_lan_gameplay_enabled(pair.matches[0]->view()), "no gameplay input before the other peer joins");
    pair.start(1);
    expect(pair.run_to(10), "the second peer joins and both run");
    expect(until([&] { return pair.matches[0]->view().phase == viewer::GalacticLanPhase::running
        && pair.matches[1]->view().phase == viewer::GalacticLanPhase::running; }, 5s), "both views are running");
    // The second peer stops at tick 10; the first one wants more and waits on it.
    pair.lives[0]->advance_to(60);
    expect(until([&] { return pair.matches[0]->view().phase == viewer::GalacticLanPhase::waiting_for_peer; }, 10s),
        "the frontier waiting on a peer is a view state");
    const auto waiting = pair.matches[0]->view();
    expect(waiting.waiting_for == std::vector<net::PeerIndex>{1} && viewer::galactic_lan_gameplay_enabled(waiting),
        "the view names the peer it waits on and still takes input");
    pair.lives[1]->advance_to(60);
    expect(pair.run_to(60), "the wait ends when the peer continues");
    expect(until([&] { return pair.matches[0]->view().phase == viewer::GalacticLanPhase::running; }, 5s), "back to running");
}

// GCMP-04: an invasion acceptance crosses the same agreed input stream as other seat commands.
// A stale offer is refused by execution on both peers, never dropped at the local adapter.
void invasion_accept_is_agreed(const std::array<std::size_t, 2> workers) {
    Pair pair(versus_setup(), workers);
    expect(pair.run_to(5), "invasion acceptance fixture runs");
    const galactic::CommandPayload accept = galactic::InvasionAccept{900};
    pair.order(0, accept);
    expect(pair.run_to(30), "a stale invasion acceptance does not disturb lockstep");
    for (auto& live : pair.lives) live->stop();
    for (const auto& live : pair.lives) {
        const auto rejected = live->rejected_orders();
        expect(rejected.size() == 1 && rejected.front().message == "invasion offer is no longer available",
            "each peer refuses the stale offer at execution");
        const auto record = live->record();
        expect(record.commands.size() == 1 && record.commands.front().payload == accept
            && record.commands.front().key.seat == pair.humans[0],
            "each peer journals the typed invasion acceptance");
    }
    const auto first = pair.lives[0]->tick_hashes();
    expect(first == pair.lives[1]->tick_hashes(), "invasion acceptance keeps both peers byte-equal");
    expect(reference(pair.setup, pair.lives[0]->record().commands, first.size()) == first,
        "the agreed invasion acceptance replays identically");
}

void departure_continues_alone() {
    Pair pair(policy_setup(), {1, 4});
    expect(pair.run_to(20), "departure fixture runs");
    pair.order(1, galactic::SeatLeave{});
    // The leaver's pacer stops once its seat departed, so only the survivor is held to the target.
    for (auto& live : pair.lives) live->advance_to(32);
    expect(pair.lives[0]->wait_for(32, 30s), "the leave executes at an agreed tick and the survivor goes on");
    expect(until([&] { return pair.matches[1]->view().phase == viewer::GalacticLanPhase::ended; }, 5s)
        && pair.matches[1]->view().reason == viewer::GalacticLanReason::local_left, "the leaver ends with local_left");
    expect(until([&] { return pair.matches[0]->view().phase == viewer::GalacticLanPhase::departed; }, 5s),
        "the survivor is in the departed state");
    const auto view = pair.matches[0]->view();
    expect(view.departed.size() == 1 && view.departed[0] == pair.humans[1], "the departed seat is named");
    expect(viewer::galactic_lan_gameplay_enabled(view), "the survivor keeps playing");
    pair.order(1, galactic::PauseGalaxy{});
    expect(until([&] { return !pair.lives[1]->rejected_orders().empty(); }, 5s),
        "orders after local_left reach rejected_orders");
    const auto rejected = pair.lives[1]->rejected_orders();
    expect(rejected.size() == 1 && rejected.front().code == "EAWR-PLATFORM-0600"
        && rejected.front().message.find("ended") != std::string::npos, "departure rejection explains the ended match");
    pair.lives[0]->advance_to(70);
    expect(pair.lives[0]->wait_for(70, 30s) && !pair.lives[0]->failure(), "the survivor continues alone");
    expect(view.message.find("Seat " + std::to_string(pair.humans[1].value)) != std::string::npos
        && view.message.find("agreed tick") != std::string::npos
        && view.message.find("continue playing or quit") != std::string::npos,
        "the survivor diagnostic identifies the departure and continuation policy");
    const auto snapshot = pair.lives[0]->frame().latest;
    expect(snapshot->seat_control() && snapshot->seat_control()->control.controllers[1].kind == galactic::FactionControl::ai,
        "D5: the published galaxy uses the departed opponent's configured AI fallback");
    expect(std::any_of(snapshot->seats().begin(), snapshot->seats().end(), [](const auto& seat) {
        return seat.player == 2 && seat.kind == eawr::sim::SeatKind::ai && seat.active;
    }), "the published fallback AI issuer is active");
    pair.order(0, galactic::SeatRejoin{});
    expect(until([&] { return !pair.lives[0]->rejected_orders().empty(); }, 5s), "a local rejoin intent is refused");
    const auto rejoin_refusals = pair.lives[0]->rejected_orders();
    expect(!rejoin_refusals.empty() && rejoin_refusals.front().code == net::session_errors::admission
        && rejoin_refusals.front().message.find("rejoin is disabled") != std::string::npos,
        "the refusal explains why Galactic rejoin is unavailable");
    pair.lives[0]->advance_to(80);
    expect(pair.lives[0]->wait_for(80, 30s) && !pair.lives[0]->failure(), "a refused rejoin leaves the campaign running");
    pair.order(0, galactic::SeatLeave{});
    pair.lives[0]->advance_to(96);
    expect(until([&] { return pair.matches[0]->view().reason == viewer::GalacticLanReason::local_left; }, 5s),
        "the remaining versus human can choose to quit after playing against the fallback");
    for (auto& live : pair.lives) live->stop();
    const auto first = pair.lives[0]->tick_hashes();
    const auto second = pair.lives[1]->tick_hashes();
    expect(!pair.lives[0]->failure() && !pair.lives[1]->failure(), "both voluntary departures finish without failure");
    expect(first.size() >= second.size() && std::equal(second.begin(), second.end(), first.begin()),
        "live peers have equal digests through the opponent's agreed departure tick");
    const auto first_commands = pair.lives[0]->record().commands;
    const auto second_commands = pair.lives[1]->record().commands;
    const auto leave = [&](const auto& command) {
        return command.key.seat == pair.humans[1] && std::holds_alternative<galactic::SeatLeave>(command.payload);
    };
    const auto first_leave = std::find_if(first_commands.begin(), first_commands.end(), leave);
    const auto second_leave = std::find_if(second_commands.begin(), second_commands.end(), leave);
    expect(first_leave != first_commands.end() && second_leave != second_commands.end()
        && first_leave->key == second_leave->key && second.size() == first_leave->key.tick + 1
        && view.message.find("agreed tick " + std::to_string(first_leave->key.tick) + ".") != std::string::npos,
        "live replay stamps and the view diagnostic name the same applied departure tick");
}

void closed_link_applies_fallback() {
    Pair pair(policy_setup(), {2, 8});
    expect(pair.run_to(20), "closed-link fixture runs");
    pair.lives[1]->stop();
    {
        const std::lock_guard lock(pair.pipes.mutex);
        pair.pipes.open_[1] = false;
    }
    pair.lives[0]->advance_to(70);
    expect(pair.lives[0]->wait_for(70, 30s) && !pair.lives[0]->failure(), "the survivor agrees a disconnect and continues");
    expect(pair.matches[0]->view().phase == viewer::GalacticLanPhase::departed
        && pair.matches[0]->view().message.find("Rejoin is disabled") != std::string::npos,
        "a closed link publishes departed and the no-rejoin diagnostic");
    const auto snapshot = pair.lives[0]->frame().latest;
    expect(snapshot->seat_control() && snapshot->seat_control()->control.controllers[1].kind == galactic::FactionControl::ai,
        "an agreed disconnect activates the configured AI fallback");
    expect(std::any_of(snapshot->seats().begin(), snapshot->seats().end(), [&](const auto& seat) {
        return seat.seat_id == pair.humans[1] && !seat.active;
    }), "the disconnected human stays inactive");
}

void install_observer(galactic::GalacticSetup& setup) {
    auto& observer = setup.observer.emplace(); observer.human_observers = {1, 2};
    for (const auto& player : setup.players) {
        galactic::ObserverPlayer facts; facts.player = player.player_id; facts.ai = player.player_id > 2;
        facts.minimum_visible_base_level = 100; observer.players.push_back(facts);
    }
    for (const auto& planet : setup.planets) {
        galactic::ObserverPlanet facts; facts.planet = planet.id;
        facts.allegiance = facts.target_owner = facts.current_owner = facts.current_controller = planet.owner;
        observer.planets.push_back(facts);
    }
    for (const auto& fleet : setup.fleets) for (const auto& member : fleet.members)
        if (std::none_of(observer.types.begin(), observer.types.end(), [&](const auto& row) { return row.type == member.type; }))
            observer.types.push_back({member.type});
    std::sort(observer.types.begin(), observer.types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    std::vector<galactic::ObserverHome> homes;
    for (const auto player : observer.human_observers) {
        const auto home = std::find_if(setup.planets.begin(), setup.planets.end(), [&](const auto& row) { return row.owner == player; });
        homes.push_back({player, home == setup.planets.end() ? std::nullopt : std::optional(home->id)});
    }
    require(galactic::initialize_campaign_observers(setup, homes));
}

void private_views_and_digests(const std::array<std::size_t, 2> workers) {
    auto initial = versus_setup(); install_observer(initial);
    Pair pair(initial, workers);
    for (const auto peer : {0U, 1U}) {
        const auto frame = pair.lives[peer]->frame(pair.humans[peer]);
        const auto player = peer + 1;
        expect(frame.latest && frame.latest->view_player() == player, "agreed live peer publishes its own human observer");
        std::vector<galactic::FleetDisplayVisibility> sight;
        for (const auto& row : frame.latest->observations()) {
            expect(row.player == player, "private live frame excludes other players' observer rows");
            sight.push_back({row.planet, row.discovered});
        }
        const auto fleets = galactic::observed_fleets(*frame.latest, player, {}, 0, sight);
        expect(!fleets.presence.empty(), "private fixture exposes the requesting human's fleet");
        for (const auto& row : fleets.presence) expect(row.owner == player, "other human's secret force stays out of tick-zero publication");
    }
    expect(pair.run_to(16), "private live peers advance the agreed frontier");
    for (auto& live : pair.lives) live->stop();
    expect(pair.lives[0]->tick_hashes() == pair.lives[1]->tick_hashes(), "different live view players have equal per-tick digests");
    expect(require(galactic::write_replay(pair.lives[0]->record())) == require(galactic::write_replay(pair.lives[1]->record())),
        "different live view players produce identical replay bytes");
}

galactic::GalacticSetup multiplayer_child_setup(const std::uint32_t players = 3) {
    auto setup = versus_setup(players);
    install_observer(setup);
    setup.victory.emplace();
    auto& context = setup.victory->context;
    context.multiplayer = true;
    context.types.push_back({"FixturePlanet", 1});
    for (const auto& player : setup.players) {
        galactic::VictoryPlayer row;
        row.player = player.player_id; row.faction = "Faction" + std::to_string(player.player_id);
        row.playable = true; row.human = player.player_id <= 2; row.ai = !row.human;
        context.players.push_back(std::move(row));
        for (const auto& other : setup.players)
            context.relations.push_back({player.player_id, other.player_id, player.player_id == other.player_id});
    }
    for (const auto& planet : setup.planets)
        context.planets.push_back({planet.id, 1, planet.owner, planet.owner, planet.owner, false});
    return setup;
}

eawr::sim::tactical::TacticalContent child_content(const eawr::campaign::ConflictRecord& entry) {
    namespace tactical = eawr::sim::tactical;
    const auto whole = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    tactical::TacticalContent content;
    // Without damage rules the legacy durability contract records projectile events but applies no shot damage.
    content.durability.damage.emplace();
    content.durability.damage->shield_recharge_frames = 90;
    for (const auto& force : entry.forces) {
        if (std::any_of(content.durability.profiles.begin(), content.durability.profiles.end(),
            [&](const auto& row) { return row.type_id == force.type; })) continue;
        tactical::DurabilityProfile hull; hull.type_id = force.type; hull.max_hull = whole(100);
        content.durability.profiles.push_back(hull);
        content.victory.relevant_types.push_back(force.type);
        tactical::CombatProfile combat;
        combat.type_id = force.type; combat.category_bits = 1;
        // No autonomous ship scan: this fixture settles only after the authenticated Attack order.
        combat.max_attack_distance = whole(0);
        combat.collision = tactical::CollisionBox{{whole(-5), whole(-5), whole(-5)}, {whole(5), whole(5), whole(5)}};
        tactical::WeaponProfile gun;
        gun.range = whole(1000); gun.cone_width = whole(360); gun.cone_height = whole(180);
        tactical::ShotProfile shot; shot.damage = whole(1000); shot.speed = whole(100); shot.max_travel = whole(1000);
        gun.shot = shot; combat.weapons.push_back(gun);
        content.combat.profiles.push_back(std::move(combat));
        content.sensors.push_back({force.type, whole(1000)});
    }
    const auto by_type = [](const auto& a, const auto& b) { return a.type_id < b.type_id; };
    std::sort(content.durability.profiles.begin(), content.durability.profiles.end(), by_type);
    std::sort(content.combat.profiles.begin(), content.combat.profiles.end(), by_type);
    std::sort(content.sensors.begin(), content.sensors.end(), by_type);
    std::sort(content.victory.relevant_types.begin(), content.victory.relevant_types.end());
    content.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    content.victory.contenders = {1, 2}; content.victory.controlled_players = {1, 2}; content.victory.humans = {1, 2};
    content.victory.countdown_frames = 2;
    return content;
}

eawr::campaign::GameSession::VoteProgress child_vote_progress() {
    return [](const auto&, const auto& votes) {
        return eawr::core::Result<std::optional<bool>>::success(votes.size() == 2 ? std::optional(true) : std::nullopt);
    };
}

bool conflict_human(const galactic::GalacticSetup& initial, const eawr::campaign::ConflictRecord& entry,
    const eawr::sim::tactical::PlayerId player) {
    return std::any_of(entry.seats.begin(), entry.seats.end(), [&](const auto& binding) {
        return binding.player == player && std::any_of(initial.seats.begin(), initial.seats.end(), [&](const auto& seat) {
            return seat.seat_id == binding.seat && seat.kind == galactic::SeatKind::human;
        });
    });
}

// Synthetic assets use production session arbitration and the real tactical AI host.
std::shared_ptr<eawr::platform::CampaignServiceConfiguration> child_services(
    const galactic::GalacticSetup& initial, const bool automatic, const bool multiplayer_votes = false,
    const std::uint64_t timeout_ticks = 450, const bool tactical_ai = false) {
    namespace cp = eawr::campaign;
    namespace tactical = eawr::sim::tactical;
    auto services = std::make_shared<eawr::platform::CampaignServiceConfiguration>();
    const auto content = [initial, tactical_ai](const cp::ConflictRecord& entry) {
        auto rules = child_content(entry);
        if (tactical_ai) std::erase_if(rules.victory.humans, [&](auto player) {
            return !conflict_human(initial, entry, player);
        });
        return eawr::core::Result<tactical::TacticalContent>::success(std::move(rules));
    };
    services->restore.content = content;
    services->configure = [initial, automatic, multiplayer_votes, timeout_ticks, tactical_ai, content](cp::GameSession& game) {
        cp::ConflictRecord entry;
        entry.id = game.galaxy().next_entity_id();
        entry.planet = initial.planets.front().id; entry.invader = 1; entry.defender = 2;
        entry.layer = automatic ? cp::BattleLayer::land : cp::BattleLayer::space;
        entry.tactical.content_identity = initial.content_identity; entry.tactical.first_free_entity_id = entry.id + 1;
        entry.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
        for (const auto owner : {1U, 2U}) {
            const auto fleet = std::find_if(initial.fleets.begin(), initial.fleets.end(), [owner](const auto& row) { return row.owner == owner; });
            const auto member = fleet->members.front();
            entry.forces.push_back({member.id, member.type, owner, fleet->id});
            tactical::UnitState unit; unit.entity_id = member.id; unit.type_id = member.type; unit.owner = owner;
            unit.position.x = math::Fixed::from_integer(owner == 1 ? -50 : 50).value();
            entry.tactical.units.push_back(unit);
        }
        std::sort(entry.forces.begin(), entry.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        std::sort(entry.tactical.units.begin(), entry.tactical.units.end(), [](const auto& a, const auto& b) { return a.entity_id < b.entity_id; });
        for (const auto& seat : initial.seats) if (seat.active && (multiplayer_votes || seat.player <= 2))
            entry.seats.push_back({seat.seat_id, seat.player <= 2 ? seat.player : 0});
        game.set_conflict_service([](const auto&, const auto&, auto) {
            return eawr::core::Result<std::optional<cp::RecordedAdmission>>::success({});
        }, content, {}, cp::ScriptRequirement::none);
        game.set_vote_progress(child_vote_progress());
        if (multiplayer_votes) {
            auto configured = game.configure_multiplayer_votes(timeout_ticks);
            if (!configured) return configured;
        }
        if (tactical_ai) {
            // The same skirmish player selection and prepared tactical host used by
            // StockSpaceEntry::prepare_ai and the LAN battle path. Only asset data is synthetic.
            auto bind = std::make_shared<cp::GameSession::ChildConfigurator>();
            auto installed = game.set_child_script_services([initial, bind](const cp::ConflictRecord& conflict) {
                eawr::battle::BattleStart start; start.setup = conflict.tactical;
                for (const auto& player : conflict.tactical.players) {
                    eawr::battle::StartPlayer row; row.player = player; row.faction = "Fixture"; row.lobby = true;
                    row.human = conflict_human(initial, conflict, player.player_id);
                    start.players.push_back(std::move(row));
                }
                auto setup = eawr::skirmish::ai_setup(start, {}, {});
                for (const auto& force : conflict.forces) if (std::none_of(setup.content.types.begin(), setup.content.types.end(),
                    [&](const auto& row) { return row.type_id == force.type; })) {
                    eawr::script::foc::AiType type; type.type_id = force.type; type.name = "Fixture";
                    type.locomotor = true; type.category_bits = 1; setup.content.types.push_back(type);
                }
                setup.freestore_module = "GcMpAiFixture.lua";
                setup.player_ai_start_ticks.emplace(2, 20);
                const std::map<std::string, std::string> modules{
                    {"GcMpAiFixture.lua", "function Base_Definitions() ServiceRate=0; UnitServiceRate=0 end\n"
                        "function main() FogOfWar.Reveal_All(PlayerObject) end\n"
                        "function On_Unit_Service(unit) local enemy=Find_Nearest(unit,PlayerObject,false); "
                        "if enemy and not unit.Has_Attack_Target() then unit.Attack_Target(enemy) end end"},
                    {"data/scripts/library/PGAICommands.lua", "function Base_Definitions() EnemyContrastTypes={}; FriendlyContrastTypes={} end"}};
                auto prepared = eawr::script::foc::prepare_session(setup, modules);
                if (!prepared) return eawr::core::Result<eawr::script::authoritative::ScriptScheduler>::failure(prepared.error());
                *bind = std::move(prepared.value().bind);
                return eawr::core::Result<eawr::script::authoritative::ScriptScheduler>::success(std::move(prepared.value().scripts));
            }, [bind](auto& child) { return (*bind)(child); });
            if (!installed) return installed;
        }
        if (automatic || multiplayer_votes) {
            game.set_auto_resolver([](const auto& conflict, const auto&, const auto&) {
                cp::AutoResolveOutcomeApplication outcome;
                outcome.planet = conflict.planet; outcome.planet_type = "FixturePlanet";
                outcome.invader = conflict.invader; outcome.defender = conflict.defender; outcome.land = conflict.layer == cp::BattleLayer::land;
                outcome.result.conflict = conflict.id; outcome.result.winner = conflict.defender;
                for (const auto& force : conflict.forces) outcome.result.forces.push_back({force.id, cp::ForceDisposition::survived});
                return eawr::core::Result<std::optional<cp::AutoResolveOutcomeApplication>>::success(outcome);
            });
            cp::AutoResolveOutcomeServices outcomes;
            outcomes.control = [](auto&, const auto&) { return eawr::core::Result<void>::success(); };
            game.set_auto_resolve_outcome_services(std::move(outcomes));
        }
        return game.queue_conflict(entry, {automatic ? cp::ConflictRoute::instant_auto : cp::ConflictRoute::pending_tactical});
    };
    return services;
}

// GCMP-06: production policy over two live owners and the real LockstepSession.
// Every comparison covers the complete per-tick digest stream, including the frozen galaxy.
void multiplayer_arbitration() {
    enum class Case { both_yes, one_no, timeout, refusal, land, ai_begin, ai_resolve, ai_timeout, no_human };
    for (const auto scenario : {Case::both_yes, Case::one_no, Case::timeout, Case::refusal, Case::land, Case::ai_begin, Case::ai_resolve,
            Case::ai_timeout, Case::no_human}) {
        const bool human_ai = scenario == Case::ai_begin || scenario == Case::ai_resolve || scenario == Case::ai_timeout;
        auto initial = multiplayer_child_setup(scenario == Case::no_human ? 4 : 3);
        if (human_ai || scenario == Case::no_human) {
            for (auto& seat : initial.seats) {
                if (seat.seat_id.value == 2) seat.player = human_ai ? 3 : 4;
                else if (seat.seat_id.value == 3) seat.player = human_ai ? 2 : 1;
                else if (seat.seat_id.value == 1 && scenario == Case::no_human) seat.player = 3;
                else if (seat.seat_id.value == 4) seat.player = 2;
            }
            initial.observer->human_observers = human_ai ? std::vector<std::uint32_t>{1, 3} : std::vector<std::uint32_t>{3, 4};
            for (auto& player : initial.observer->players) player.ai = std::find(initial.observer->human_observers.begin(),
                initial.observer->human_observers.end(), player.player) == initial.observer->human_observers.end();
            for (auto& player : initial.victory->context.players) {
                player.human = std::find(initial.observer->human_observers.begin(), initial.observer->human_observers.end(), player.player)
                    != initial.observer->human_observers.end(); player.ai = !player.human;
            }
            initial.seat_control.emplace();
            initial.seat_control->teams = {{1, {1}, false}, {2, {2}, false}};
            for (const auto& player : initial.players) {
                const bool human = std::find(initial.observer->human_observers.begin(), initial.observer->human_observers.end(), player.player_id)
                    != initial.observer->human_observers.end();
                initial.seat_control->controllers.push_back({player.player_id,
                    human ? galactic::FactionControl::human : galactic::FactionControl::ai, galactic::FactionControl::ai, {}, {}});
            }
        }
        Pair pair(initial, {1, 4}, LiveGameSession::Pacing::driven, 3000, 0);
        const auto services = child_services(initial, scenario == Case::land, true, 450, human_ai);
        const auto configure = [services](auto& options) { options.services = services; };
        pair.start(0, configure); pair.start(1, configure);
        expect(pair.run_to(10), "MP arbitration reaches the pending boundary");
        const bool instant = scenario == Case::land || scenario == Case::no_human;
        if (instant) {
            for (auto& live : pair.lives) expect(!live->frame().pending_conflict && live->frame().auto_resolve_outcome,
                "land and no-human MP conflicts auto-resolve without a vote");
            expect(pair.lives[0]->record().commands.empty(), "instant resolution needs no local choice");
            const auto involved = pair.lives[0]->frame(pair.humans[0]);
            const auto other = pair.lives[1]->frame(pair.humans[1]);
            expect(bool(involved.auto_resolve_outcome) == (scenario == Case::land), "WAR-68 only an involved seat receives the result notice");
            expect(bool(other.auto_resolve_outcome) == (scenario == Case::land), "WAR-68 excludes the uninvolved seat");
            eawr::viewer::galactic::LiveBattleDialogNames names;
            names.planet = [](auto) { return std::string("FixturePlanet"); };
            names.side = [](auto) { return eawr::viewer::galactic::BattleDialogSide{}; };
            const auto notice = eawr::viewer::galactic::live_battle_dialog_view(involved, pair.humans[0], 1, {}, names);
            expect(scenario == Case::land ? notice.phase == eawr::viewer::galactic::BattleDialogPhase::summary && notice.auto_resolved
                : notice.phase == eawr::viewer::galactic::BattleDialogPhase::hidden, "WAR-68 result UI is involved-seat only");
        } else if (human_ai) {
            // Owner update 10-21: a human fighting the AI opens the same Begin / Auto-Resolve vote; only the human side votes.
            const auto pending = pair.lives[0]->frame().pending_conflict;
            expect(pending && pair.lives[1]->frame().pending_conflict && pending->arbitration.route == eawr::campaign::ConflictRoute::vote
                && !pair.lives[0]->frame().battle, "human-versus-AI space opens the session vote on both peers");
            if (!pending) continue;
            const auto conflict = pending->conflict.id;
            const auto frozen = pair.galaxy_frame(0);
            const auto view = pair.lives[0]->frame(pair.humans[0]).pending_conflict;
            expect(view && view->alert && view->alert->visible && view->alert->begin_admitted && view->alert->resolve_admitted
                && !view->alert->waiting_for_opponent && view->alert->timeout_ms == 15000,
                "the lone human side sees the full Begin / Auto-Resolve prompt, not an opponent wait");
            const auto uninvolved = pair.lives[1]->frame(pair.humans[1]).pending_conflict;
            expect(!uninvolved || !uninvolved->alert, "a human outside the conflict gets no prompt");
            if (scenario == Case::ai_timeout) {
                expect(pair.run_to(452), "session ticks continue through timeout equality");
                expect(pair.lives[0]->frame().pending_conflict != nullptr, "450 elapsed ticks do not fire the strict timeout");
                expect(pair.run_to(460), "agreed timeout defaults the human side to Begin");
                expect(pair.lives[0]->record().commands.empty(), "the AI-fight timeout default needs no peer-local vote");
            } else {
                expect(pair.run_to(20) && pair.lives[0]->frame().pending_conflict && pair.galaxy_frame(0) == frozen,
                    "the vote holds the campaign clock until the human chooses");
                pair.order(0, galactic::BattleChoice{conflict, scenario == Case::ai_begin});
                expect(pair.run_to(30), "the single human side's choice settles without waiting for the AI");
            }
            if (scenario == Case::ai_resolve) {
                expect(!pair.lives[0]->frame().battle && !pair.lives[1]->frame().battle && !pair.lives[0]->frame().pending_conflict,
                    "Auto-Resolve against the AI resolves instantly on both peers");
                expect(pair.lives[0]->frame(pair.humans[0]).auto_resolve_outcome && !pair.lives[1]->frame(pair.humans[1]).auto_resolve_outcome,
                    "WAR-68 the involved human gets the result notice, the uninvolved human does not");
                expect(pair.run_to(60) && pair.galaxy_frame(0) > frozen, "the campaign clock resumes after the instant result");
            } else {
                const auto frame = pair.lives[0]->frame();
                expect(frame.battle && pair.lives[1]->frame().battle && frame.conflict && !frame.pending_conflict,
                    "Begin or the timeout default launches the tactical child on both peers");
                expect(frame.latest->seat_control() && frame.latest->seat_control()->battle
                    && frame.latest->seat_control()->battle->role == galactic::BattleTeamRole::spectate,
                    "D10 uninvolved team spectates instead of blocking the AI child on a pending role dialog");
                if (frame.conflict) {
                    const auto feed = pair.lives[0]->battle_feed(frame.conflict->id, pair.humans[0]);
                    const auto inside = pair.galaxy_frame(0);
                    bool ai_attack{};
                    for (std::uint64_t tick = frame.session_tick + 1; tick <= frame.session_tick + 60; ++tick) {
                        expect(pair.run_to(tick), "D10 child AI and return advance through agreed ticks");
                        if (feed) for (const auto& command : feed->record().commands)
                            ai_attack = ai_attack || (command.key.player_id == 2
                                && std::holds_alternative<eawr::sim::tactical::AttackPayload>(command.payload));
                        if (!pair.lives[0]->frame().conflict) break;
                        expect(pair.galaxy_frame(0) == inside, "the campaign clock stays frozen while the child runs");
                    }
                    const auto result = pair.lives[0]->frame();
                    expect(ai_attack, "D10 AI-side freestore issued an actual tactical Attack order");
                    expect(!result.conflict && result.result && result.result->winner == 2,
                        "D10 tactical AI fought and won without peer-local or human AI-side orders");
                }
            }
        } else {
            const auto pending = pair.lives[0]->frame().pending_conflict;
            expect(pending && pending->arbitration.route == eawr::campaign::ConflictRoute::vote, "two-human space conflict opens session vote");
            if (!pending) continue;
            const auto conflict = pending->conflict.id;
            const auto frozen = pair.galaxy_frame(0);
            const auto view = pair.lives[0]->frame(pair.humans[0]).pending_conflict;
            expect(view && view->alert && view->alert->visible && view->alert->timeout_ms == 15000
                && !view->alert->timeout_due, "vote UI reads the injected timer without a local timeout order");
            if (scenario == Case::timeout) {
                expect(pair.run_to(452), "session ticks continue through timeout equality");
                expect(pair.lives[0]->frame().pending_conflict != nullptr, "450 elapsed ticks do not fire the strict timeout");
                expect(pair.run_to(460), "agreed timeout selects Begin");
                expect(pair.lives[0]->record().commands.empty(), "timeout is canonical session progress without peer-local votes");
            } else {
                if (scenario == Case::refusal) pair.order(0, galactic::BattleChoice{conflict + 100, true});
                pair.order(0, galactic::BattleChoice{conflict, scenario != Case::one_no});
                if (scenario == Case::one_no) {
                    expect(pair.run_to(20), "one refusal settles without waiting for the other side");
                    expect(pair.galaxy_frame(0) > frozen, "GCMP-10 regression: strategic time advances past human-versus-human conflict");
                } else {
                    expect(pair.run_to(20), "first vote waits for opponent");
                    const auto waiting = pair.lives[0]->frame(pair.humans[0]).pending_conflict;
                    expect(waiting && waiting->alert && waiting->alert->waiting_for_opponent, "voting seat sees confirmed waiting state");
                    pair.order(0, galactic::BattleChoice{conflict, false}); // duplicate side vote is an agreed refusal
                    pair.order(1, galactic::BattleChoice{conflict, true});
                }
                expect(pair.run_to(30), "explicit votes settle on the agreed frontier");
            }
            if (scenario == Case::one_no) {
                expect(!pair.lives[0]->frame().battle && pair.lives[0]->frame().auto_resolve_outcome,
                    "a no vote deterministically auto-resolves");
            } else expect(pair.lives[0]->frame().battle && pair.lives[1]->frame().battle, "two yes votes or timeout open the child on both peers");
            if (scenario == Case::refusal) expect(!pair.lives[0]->rejected_orders().empty(), "stale conflict and duplicate votes are refused");
            if (scenario == Case::both_yes || scenario == Case::timeout) {
                const auto frame = pair.lives[0]->frame();
                const auto feed = pair.lives[0]->battle_feed(conflict, pair.humans[0]);
                expect(frame.conflict && feed, "voted child exposes its authenticated tactical feed");
                if (frame.conflict && feed) {
                    const auto own = std::find_if(frame.conflict->forces.begin(), frame.conflict->forces.end(),
                        [](const auto& row) { return row.owner == 1; })->id;
                    const auto target = std::find_if(frame.conflict->forces.begin(), frame.conflict->forces.end(),
                        [](const auto& row) { return row.owner == 2; })->id;
                    feed->submit({1, {own}, eawr::sim::tactical::AttackPayload{target}, {}});
                    expect(pair.run_to(frame.session_tick + 60), "voted child fights and returns through agreed ticks");
                    expect(!pair.lives[0]->frame().conflict && !pair.lives[1]->frame().conflict
                        && pair.galaxy_frame(0) > frozen && pair.galaxy_frame(0) == pair.galaxy_frame(1),
                        "GCMP-10 regression: strategic clock resumes after the human-versus-human battle");
                }
            }
        }
        for (auto& live : pair.lives) live->stop();
        expect(!pair.lives[0]->failure() && !pair.lives[1]->failure(), "arbitration never fails the network session");
        expect(pair.lives[0]->tick_hashes() == pair.lives[1]->tick_hashes(), "arbitration and refusals have equal digests every tick");
    }
    // A nonstock injected timeout, an already sealed yes vote and its start tick
    // survive direct checkpoint import. Restore must not restart the countdown.
    const auto setup = multiplayer_child_setup();
    const auto services = child_services(setup, false, true, 19);
    auto original = require(eawr::campaign::GameSession::create(setup));
    require(services->configure(original));
    const eawr::platform::ThreadWorkerAdapter single(1), parallel(4);
    for (unsigned tick = 0; tick < 10; ++tick) require(original.step(single));
    const auto conflict = original.pending_conflict()->id;
    require(original.submit({{10, {1}, 1}, galactic::BattleChoice{conflict, true}}));
    require(original.step(single));
    const auto owner = require(original.save_checkpoint_state());
    const auto recording = original.recording();
    auto restored = require(eawr::campaign::GameSession::from_recording(recording, services->restore.content, {},
        eawr::campaign::ScriptRequirement::none));
    require(restored.load_checkpoint_state(owner));
    expect(original.state_sha256() == restored.state_sha256(), "pending vote checkpoint preserves the exact barrier digest");
    const auto identities = eawr::checkpoint::game_identities(setup);
    const auto saved = require(eawr::checkpoint::save_game_checkpoint(original, identities));
    auto load = services->restore; load.script_requirement = eawr::campaign::ScriptRequirement::none;
    auto imported = require(eawr::checkpoint::load_game_checkpoint(saved, identities, load, parallel));
    expect(imported.session.state_sha256() == original.state_sha256(), "public checkpoint restores the vote policy without a legacy callback");
    for (unsigned tick = 11; tick < 30; ++tick) {
        const auto first = require(original.step(single));
        const auto second = require(restored.step(parallel));
        const auto third = require(imported.session.step(parallel));
        expect(first.state_hash.get() == second.state_hash.get(), "restored vote and nonstock timeout match every tick at 1/4 workers");
        expect(first.state_hash.get() == third.state_hash.get(), "public checkpoint preserves the pending vote deadline at every tick");
    }
    expect(original.battle() && restored.battle(), "restored timeout defaults only the missing side and selects Begin");
}

// GCMP-06 review: only multiplayer filters the result notice per seat. A solo session keeps the
// notice for an auto-resolved conflict its human seats are not part of.
void solo_keeps_every_auto_resolve_notice() {
    auto initial = multiplayer_child_setup(3);
    initial.victory->context.multiplayer = false;
    for (auto& seat : initial.seats) {
        if (seat.seat_id.value == 2) seat.player = 3;
        else if (seat.seat_id.value == 3) seat.player = 2;
    }
    initial.observer->human_observers = {1, 3};
    for (auto& player : initial.observer->players) player.ai = std::find(initial.observer->human_observers.begin(),
        initial.observer->human_observers.end(), player.player) == initial.observer->human_observers.end();
    for (auto& player : initial.victory->context.players) {
        player.human = std::find(initial.observer->human_observers.begin(), initial.observer->human_observers.end(), player.player)
            != initial.observer->human_observers.end(); player.ai = !player.human;
    }
    const auto services = child_services(initial, true);
    LiveGameSession::Options options;
    options.workers = 1; options.pacing = LiveGameSession::Pacing::driven; options.services = services;
    auto live = require(LiveGameSession::start(initial, std::move(options)));
    live->advance_to(10);
    expect(live->wait_for(10, 30s) && !live->failure(), "solo auto-resolve fixture advances");
    for (const auto seat : human_seats(initial)) {
        const auto frame = live->frame(seat);
        expect(bool(frame.auto_resolve_outcome), "solo keeps the auto-resolve result notice for every human seat, involved or not");
    }
    live->stop();
}

void multiplayer_vote_departure() {
    namespace cp = eawr::campaign;
    namespace tactical = eawr::sim::tactical;
    auto initial = multiplayer_child_setup();
    const auto controls = policy_setup();
    initial.seats = controls.seats; initial.seat_control = controls.seat_control;
    {
        Pair pair(initial, {1, 4}, LiveGameSession::Pacing::driven, 3000, 0);
        const auto services = child_services(initial, false, true, 450, true);
        const auto configure = [services](auto& options) { options.services = services; };
        pair.start(0, configure); pair.start(1, configure);
        expect(pair.run_to(10), "departure peers reach an unresolved two-human vote");
        expect(pair.lives[0]->frame().pending_conflict && pair.lives[1]->frame().pending_conflict,
            "both peers are waiting before the departure");
        const auto frozen = pair.galaxy_frame(0);
        const auto vote = pair.lives[0]->frame().pending_conflict->conflict.id;
        pair.order(1, galactic::SeatLeave{});
        for (auto& live : pair.lives) live->advance_to(32);
        expect(pair.lives[0]->wait_for(32, 30s), "the survivor advances after the agreed mid-vote departure");
        expect(until([&] { return pair.matches[1]->view().reason == viewer::GalacticLanReason::local_left; }),
            "the departing peer completes the same transition frontier before stopping");
        const auto waiting = pair.lives[0]->frame();
        expect(!waiting.battle && waiting.pending_conflict && waiting.latest->seat_control()
            && waiting.latest->seat_control()->control.controllers[1].kind == galactic::FactionControl::ai,
            "the departure turns the conflict into a human-versus-AI vote that the surviving human still decides");
        expect(pair.galaxy_frame(0) == frozen, "the campaign clock stays frozen while the survivor decides");
        pair.order(0, galactic::BattleChoice{vote, true});
        pair.lives[0]->advance_to(40);
        expect(pair.lives[0]->wait_for(40, 30s), "the survivor's Begin settles the human-versus-AI vote alone");
        const auto frame = pair.lives[0]->frame();
        expect(frame.battle && frame.conflict && !frame.pending_conflict,
            "D10 the survivor's Begin selects the human-versus-AI child using the post-SeatLeave controller state");
        if (frame.conflict) {
            const auto seats = frame.latest->seats();
            expect(std::all_of(frame.conflict->seats.begin(), frame.conflict->seats.end(), [&](const auto& binding) {
                return std::any_of(seats.begin(), seats.end(), [&](const auto& seat) {
                    return seat.seat_id == binding.seat && seat.active;
                });
            }), "the selected child has no inactive seat binding");
            expect(std::any_of(frame.conflict->seats.begin(), frame.conflict->seats.end(), [](const auto& binding) {
                return binding.seat.value == 5 && binding.player == 2;
            }), "the actual active fallback seat binds the AI side");
            const auto feed = pair.lives[0]->battle_feed(frame.conflict->id, pair.humans[0]);
            bool ai_attack{};
            for (std::uint64_t tick = 41; tick <= 100; ++tick) {
                pair.lives[0]->advance_to(tick);
                expect(pair.lives[0]->wait_for(tick, 30s), "the departed opponent's tactical AI continues alone");
                if (feed) for (const auto& command : feed->record().commands)
                    ai_attack = ai_attack || (command.key.player_id == 2 && std::holds_alternative<tactical::AttackPayload>(command.payload));
            }
            const auto returned = pair.lives[0]->frame();
            expect(ai_attack && !returned.conflict && returned.result && returned.result->winner == 2
                && pair.galaxy_frame(0) > frozen, "the fallback AI fights, returns and releases strategic time");
        }
        for (auto& live : pair.lives) live->stop();
        const auto first = pair.lives[0]->tick_hashes(), second = pair.lives[1]->tick_hashes();
        const auto commands = pair.lives[0]->record().commands;
        const auto leave = std::find_if(commands.begin(), commands.end(), [](const auto& command) {
            return std::holds_alternative<galactic::SeatLeave>(command.payload);
        });
        expect(!pair.lives[0]->failure() && !pair.lives[1]->failure(), "mid-vote departure never aborts a peer");
        expect(leave != commands.end() && second.size() == leave->key.tick + 1 && first.size() >= second.size()
            && std::equal(second.begin(), second.end(), first.begin()),
            "every shared digest matches through the agreed departure and tactical selection frontier");
        expect(std::count_if(commands.begin(), commands.end(), [](const auto& command) {
            return std::holds_alternative<galactic::BattleChoice>(command.payload);
        }) == 1, "the live seat state, not a stale voter set, decided who still votes");
    }
    // Rejoin is disabled by the LAN transport. These small sealed-input cases
    // cover its simulation rule before vote resolution, and the zero-human tail.
    for (const bool rejoin : {true, false}) {
        const auto services = child_services(initial, false, true);
        std::array<cp::GameSession, 2> owners{require(cp::GameSession::create(initial)), require(cp::GameSession::create(initial))};
        const eawr::platform::ThreadWorkerAdapter single(1), parallel(4);
        for (auto& owner : owners) require(services->configure(owner));
        for (unsigned tick = 0; tick < 10; ++tick) { require(owners[0].step(single)); require(owners[1].step(parallel)); }
        for (auto& owner : owners) {
            const auto id = owner.pending_conflict()->id;
            if (rejoin) {
                require(owner.submit({{10, {1}, 1}, galactic::BattleChoice{id, true}}));
                require(owner.submit({{10, {2}, 1}, galactic::SeatLeave{}}));
                require(owner.submit({{10, {2}, 2}, galactic::SeatRejoin{}}));
                require(owner.submit({{10, {2}, 3}, galactic::BattleChoice{id, true}}));
            } else {
                require(owner.submit({{10, {1}, 1}, galactic::SeatLeave{}}));
                require(owner.submit({{10, {2}, 1}, galactic::SeatLeave{}}));
            }
        }
        for (unsigned tick = 10; tick < 20; ++tick) {
            const auto a = require(owners[0].step(single)), b = require(owners[1].step(parallel));
            expect(a.state_hash.get() == b.state_hash.get(), "rejoin and no-human resolution match every tick at 1/4 workers");
        }
        if (rejoin) {
            const auto* child = owners[0].conflict();
            expect(child && std::any_of(child->seats.begin(), child->seats.end(), [](const auto& binding) {
                return binding.seat.value == 2 && binding.player == 2;
            }), "a rejoin before resolution restores the human binding from current session seats");
        } else expect(!owners[0].battle() && !owners[0].pending_conflict() && owners[0].latest_auto_resolve_outcome(),
            "no human left in the conflict selects D10 automatic resolution");
    }
    // An initially one-human admission can wait before selection too. The
    // current controllers must decide even when its original route was tactical.
    for (const bool rejoin : {true, false}) {
        const auto services = child_services(initial, false, true);
        std::array<cp::GameSession, 2> owners{require(cp::GameSession::create(initial)), require(cp::GameSession::create(initial))};
        const eawr::platform::ThreadWorkerAdapter single(1), parallel(4);
        bool select{};
        for (auto& owner : owners) {
            require(services->configure(owner));
            owner.set_conflict_gate([&](const auto&) { return select; });
            require(owner.submit({{0, {2}, 1}, galactic::SeatLeave{}}));
        }
        for (unsigned tick = 0; tick < 10; ++tick) {
            const auto a = require(owners[0].step(single)), b = require(owners[1].step(parallel));
            expect(a.state_hash.get() == b.state_hash.get(), "delayed one-human admission matches at 1/4 workers");
        }
        expect(owners[0].pending_conflict() && !owners[0].battle(), "one-human admission is held before selection");
        for (auto& owner : owners) {
            if (rejoin) require(owner.submit({{10, {2}, 2}, galactic::SeatRejoin{}}));
            else require(owner.submit({{10, {1}, 1}, galactic::SeatLeave{}}));
        }
        select = true;
        for (unsigned tick = 10; tick < 20; ++tick) {
            const auto a = require(owners[0].step(single)), b = require(owners[1].step(parallel));
            expect(a.state_hash.get() == b.state_hash.get(), "delayed admission selects from current seats with equal digests");
        }
        if (rejoin) expect(owners[0].pending_conflict() && !owners[0].battle() && !owners[0].latest_auto_resolve_outcome(),
            "a second human joining before selection converts the tactical admission to a vote");
        else expect(!owners[0].pending_conflict() && !owners[0].battle() && owners[0].latest_auto_resolve_outcome(),
            "the last human leaving before selection converts the tactical admission to automatic resolution");
    }
}

// Co-op: two seats of one human side hold player 1 against the AI factions (roster rule: one faction per side).
galactic::GalacticSetup coop_child_setup() {
    auto setup = multiplayer_child_setup(3);
    for (auto& seat : setup.seats) if (seat.seat_id.value == 2) { seat.kind = eawr::sim::SeatKind::ai; seat.human_side_id = 0; }
    setup.seats.push_back({{4}, 1, eawr::sim::SeatKind::human, 3, true, 1});
    setup.seats.push_back({{5}, 1, eawr::sim::SeatKind::ai, 0, false, 0}); // the side's fallback issuer
    std::sort(setup.seats.begin(), setup.seats.end(), [](const auto& a, const auto& b) { return a.seat_id < b.seat_id; });
    setup.observer->human_observers = {1};
    for (auto& player : setup.observer->players) player.ai = player.player != 1;
    for (auto& player : setup.victory->context.players) { player.human = player.player == 1; player.ai = !player.human; }
    setup.seat_control.emplace();
    setup.seat_control->teams = {{1, {1}, false}};
    for (const auto& player : setup.players)
        setup.seat_control->controllers.push_back({player.player_id,
            player.player_id == 1 ? galactic::FactionControl::human : galactic::FactionControl::ai, galactic::FactionControl::ai, {}, {}});
    return setup;
}

// GCMP co-op, campaign side (owner update 10-21): a conflict between the co-op side and an AI faction opens the same
// Begin / Auto-Resolve vote as human-versus-human. Only the human side votes and the first choice of any of its seats is
// the side's vote (a teammate's later choice is refused). Begin or the 15 s default opens the tactical child with every
// co-op seat bound to the side's tactical player (a seat leaving keeps the other bound); Auto-Resolve resolves instantly.
// The strategic clock never advances on a tick that begins inside the vote or the battle.
enum class CoopVote { begin, resolve, timeout };
void multiplayer_coop_side_vote(const CoopVote mode) {
    namespace cp = eawr::campaign;
    const auto initial = coop_child_setup();
    const auto services = child_services(initial, false, true, 450, true);
    std::array<cp::GameSession, 2> owners{require(cp::GameSession::create(initial)), require(cp::GameSession::create(initial))};
    const eawr::platform::ThreadWorkerAdapter single(1), parallel(4);
    for (auto& owner : owners) require(services->configure(owner));
    const auto frame_of = [](const cp::GameSession& owner) { return owner.galaxy().snapshot()->clock().galactic_frame; };
    const auto bound = [](const cp::GameSession& owner, const std::uint32_t seat, const std::uint32_t player) {
        const auto* child = owner.conflict();
        return child && std::any_of(child->seats.begin(), child->seats.end(), [&](const auto& binding) {
            return binding.seat.value == seat && binding.player == player;
        });
    };
    bool prompted{}, voted{}, checked_responses{}, opened{}, left{}, returned{};
    unsigned prompt_ticks{};
    std::uint64_t frozen{};
    std::uint64_t previous = frame_of(owners[0]);
    bool previous_inside{};
    for (unsigned tick = 0; tick < 700 && !returned; ++tick) {
        const auto a = require(owners[0].step(single)), b = require(owners[1].step(parallel));
        expect(a.state_hash.get() == b.state_hash.get(), "co-op campaign matches at 1/4 workers every tick");
        const auto frame = frame_of(owners[0]);
        // The Auto-Resolve tick itself releases the vote's pause, so the galaxy steps once that tick.
        const bool resolving = mode == CoopVote::resolve && voted && !checked_responses;
        if (previous_inside && !resolving) expect(frame == previous, "the strategic clock never advances on a tick that began inside the vote or battle (mode "
            + std::to_string(static_cast<int>(mode)) + ", tick " + std::to_string(tick) + ")");
        previous = frame;
        previous_inside = owners[0].battle() || owners[0].galaxy().snapshot()->clock().pause_depth > 0;
        if (voted && !checked_responses) {
            checked_responses = true;
            const auto refused = [&](const std::uint32_t seat) {
                return std::any_of(a.battle_choice_responses.begin(), a.battle_choice_responses.end(),
                    [&](const auto& response) { return response.seat.value == seat && !response.refusal.empty(); });
            };
            expect(mode == CoopVote::begin ? refused(4) && !refused(1) : !refused(4),
                "the first teammate's choice is the side's vote and a later teammate choice is refused");
        }
        if (!prompted) {
            const auto view = owners[0].pending_conflict_view({1});
            if (view && view->choice_ready) {
                prompted = true; frozen = frame;
                expect(view->arbitration.route == cp::ConflictRoute::vote && !owners[0].battle() && owners[1].pending_conflict(),
                    "co-op side versus AI opens the Begin / Auto-Resolve vote on both peers");
                for (const std::uint32_t seat : {1U, 4U}) {
                    const auto mate = owners[0].pending_conflict_view({seat});
                    expect(mate && mate->alert && mate->alert->visible && mate->alert->begin_admitted && mate->alert->resolve_admitted
                        && !mate->alert->waiting_for_opponent && mate->alert->timeout_ms == 15000,
                        "each co-op teammate sees the full prompt, with no opponent wait on the AI side");
                }
            }
        } else if (!voted && mode != CoopVote::timeout && ++prompt_ticks == 5) {
            expect(frame_of(owners[0]) == frozen && owners[0].pending_conflict() && !owners[0].battle(),
                "the vote holds the campaign clock while the teammates decide");
            voted = true;
            for (auto& owner : owners) {
                const auto id = owner.pending_conflict()->id;
                if (mode == CoopVote::begin) {
                    require(owner.submit({{owner.completed_tick(), {1}, 1}, galactic::BattleChoice{id, true}}));
                    require(owner.submit({{owner.completed_tick(), {4}, 1}, galactic::BattleChoice{id, false}}));
                } else require(owner.submit({{owner.completed_tick(), {4}, 1}, galactic::BattleChoice{id, false}}));
            }
        }
        if (mode == CoopVote::resolve) {
            if (voted && checked_responses) {
                expect(!owners[0].battle() && !owners[0].pending_conflict() && owners[0].latest_auto_resolve_outcome()
                    && !owners[1].battle(), "a teammate's Auto-Resolve resolves the co-op conflict instantly");
                returned = true;
            }
            continue;
        }
        if (owners[0].battle() && !opened) {
            opened = true; frozen = frame;
            expect(!owners[0].pending_conflict() && owners[1].battle(), "Begin or the default vote opens the tactical child on both peers");
            if (mode == CoopVote::timeout) expect(owners[0].completed_tick() > 450, "the default fires only after the authored 15 s");
            expect(bound(owners[0], 1, 1) && bound(owners[0], 4, 1),
                "both co-op seats are bound to the side's tactical player");
            expect(bound(owners[0], 2, 2) && !bound(owners[0], 3, 3) && !bound(owners[0], 3, 0),
                "the AI defender is bound and the uninvolved AI faction is not");
            expect(!owners[0].galaxy().seat_control()->battle, "a co-op side has no teammate waiting on or spectating a role");
            for (const std::uint32_t seat : {1U, 4U}) {
                const auto view = owners[0].battle_view({seat});
                expect(view && view.value().commandable && view.value().perspective == 1,
                    "each co-op seat commands the side's tactical player");
            }
            expect(owners[0].battle_controller(1) == galactic::FactionControl::human,
                "the co-op faction is human-controlled in the battle");
            for (auto& owner : owners) require(owner.submit({{owner.completed_tick(), {4}, 2}, galactic::SeatLeave{}}));
        } else if (opened && !left && !owners[0].galaxy().seats().empty() && !owners[0].galaxy().seats()[3].active) {
            left = true;
            expect(owners[0].battle() && bound(owners[0], 1, 1) && !bound(owners[0], 4, 1),
                "a co-op seat leaving keeps its teammate bound and the battle running");
            expect(owners[0].battle_controller(1) == galactic::FactionControl::human, "the remaining teammate keeps control");
            expect(frame_of(owners[0]) == frozen, "the campaign still waits for the remaining teammate");
        }
        if (opened && !owners[0].battle()) returned = true;
    }
    expect(prompted && returned && (mode == CoopVote::resolve || (opened && left)),
        "the co-op conflict voted, then fought (losing a seat) or resolved and returned to the campaign");
    for (unsigned tick = 0; tick < 30; ++tick) { require(owners[0].step(single)); require(owners[1].step(parallel)); }
    expect(frame_of(owners[0]) > frozen && (mode == CoopVote::resolve ? owners[0].latest_auto_resolve_outcome() != nullptr
        : owners[0].latest_tactical_result() != nullptr), "the campaign resumes after the co-op conflict resolves");
}
void multiplayer_coop_side_fights_the_ai() {
    for (const auto mode : {CoopVote::begin, CoopVote::resolve, CoopVote::timeout}) multiplayer_coop_side_vote(mode);
}

void child_battle_uses_the_agreed_frontier(const std::array<std::size_t, 2> workers) {
    namespace tactical = eawr::sim::tactical;
    const auto initial = multiplayer_child_setup();
    // This pacing/checkpoint regression deliberately queues rapid toggles. The default budget is covered separately.
    Pair pair(initial, workers, LiveGameSession::Pacing::driven, 3000, 0, net::max_session_ticks,
        net::GalacticClockPolicy::unrestricted);
    const auto services = child_services(initial, false);
    const auto configure = [services](LiveGameSession::Options& options) { options.services = services; };
    pair.start(0, configure); pair.start(1, configure);
    const auto label = "child workers " + std::to_string(workers[0]) + "/" + std::to_string(workers[1]) + ": ";
    expect(pair.run_to(10), label + "shared conflict admission waits for the vote");
    const auto pending = pair.lives[0]->frame().pending_conflict;
    expect(pending && pair.lives[1]->frame().pending_conflict, label + "both peers publish the pending conflict");
    if (!pending) return;
    const auto conflict = pending->conflict.id;
    pair.order(0, galactic::BattleChoice{conflict, true});
    pair.order(1, galactic::BattleChoice{conflict, true});
    std::optional<std::uint64_t> begin;
    for (std::uint64_t tick = 11; tick <= 20; ++tick) {
        expect(pair.run_to(tick), label + "vote and Begin tick complete");
        const auto a = pair.lives[0]->frame(); const auto b = pair.lives[1]->frame();
        expect(bool(a.conflict) == bool(b.conflict), label + "view switches at the same agreed Begin tick");
        if (!begin && a.conflict) {
            begin = tick;
            expect(a.conflict->id == conflict && b.conflict->id == conflict && a.battle && b.battle
                && a.battle->completed_tick() == 0 && b.battle->completed_tick() == 0,
                label + "Begin key is the shared tick and conflict ID, with child tick zero");
        }
    }
    expect(begin.has_value(), label + "the fought space battle opened");
    if (!begin) return;
    const auto frame = pair.lives[0]->frame();
    const auto frozen_galaxy = pair.galaxy_frame(0);
    std::array<std::shared_ptr<eawr::platform::TacticalFeed>, 2> feeds;
    for (net::PeerIndex peer = 0; peer < 2; ++peer) feeds[peer] = pair.lives[peer]->battle_feed(conflict, pair.humans[peer]);
    expect(feeds[0] && feeds[1], label + "both human seats have the published tactical feed");
    if (!feeds[0] || !feeds[1]) return;
    // A truncated nested replay key is refused before routing/publication can read its producer sequence.
    pair.order(0, galactic::TacticalLayerCommand{conflict, 5, std::vector<std::uint8_t>(19)});
    expect(feeds[0]->agreed_pacing() && feeds[1]->agreed_pacing(), label + "only the shared child publishes agreed pacing");
    namespace ui = eawr::presentation::ui;
    auto scheduler = std::make_shared<ui::CommandScheduler>(2, 0, 0, [feed = std::weak_ptr(feeds[1])] {
        const auto source = feed.lock(); return source ? source->completed_tick() : 0U;
    });
    scheduler->present_snapshot(feeds[1]->completed_tick());
    auto acknowledged = std::make_shared<std::atomic<std::uint64_t>>(std::numeric_limits<std::uint64_t>::max());
    auto execution_tick = std::make_shared<std::atomic<std::uint64_t>>(0);
    feeds[1]->set_execution_observer([producer = std::weak_ptr(scheduler), acknowledged, execution_tick](
        const std::uint64_t sequence, const std::uint64_t tick) {
        if (const auto sink = producer.lock()) sink->note_execution(sequence, tick);
        execution_tick->store(tick); acknowledged->store(sequence);
    });
    feeds[1]->command_source([scheduler](const std::uint64_t tick) { return scheduler->take(tick); });
    const auto second_own = std::find_if(frame.conflict->forces.begin(), frame.conflict->forces.end(),
        [](const auto& row) { return row.owner == 2; })->id;
    const auto issue_stop = [&] {
        ui::TacticalIntent intent; intent.verb = ui::TacticalVerb::stop; intent.units = {second_own};
        const auto issued = scheduler->issue(intent);
        if (!issued) throw std::runtime_error(issued.error().message);
    };
    for (net::PeerIndex peer = 0; peer < 2; ++peer) {
        const auto force = std::find_if(frame.conflict->forces.begin(), frame.conflict->forces.end(),
            [peer](const auto& row) { return row.owner == peer + 1; });
        if (peer == 0) feeds[peer]->submit({peer + 1, {force->id}, tactical::StopPayload{}, {}});
        else issue_stop();
    }
    feeds[1]->set_paused(true);
    feeds[1]->set_paused(false); // a local menu may close before its pause has reached the agreed frontier
    feeds[1]->set_paused(true);
    expect(pair.run_to(35), label + "battle pause is agreed input");
    expect(feeds[0]->paused() && feeds[1]->paused(), label + "both feeds read the agreed pause");
    const auto paused_tick = pair.lives[0]->frame().battle->completed_tick();
    const auto paused_hash_count = feeds[0]->tick_hashes().size();
    // Deliberately send an order while paused: it must survive until the child resumes.
    const auto own = std::find_if(frame.conflict->forces.begin(), frame.conflict->forces.end(),
        [](const auto& row) { return row.owner == 1; })->id;
    // A repair reserves a UI action even when the synthetic ship has no repairable hardpoint.
    // Its execution refusal must retire that reservation only after the shared child tick.
    ui::TacticalIntent repair; repair.verb = ui::TacticalVerb::repair_hardpoint; repair.units = {second_own}; repair.hardpoint = 0;
    const auto issued_repair = scheduler->issue(repair);
    if (!issued_repair) throw std::runtime_error(issued_repair.error().message);
    feeds[1]->set_target_rate(60);
    expect(pair.run_to(45), label + "session ticks continue to carry controls while child is paused");
    for (auto& live : pair.lives) {
        expect(live->frame().battle && live->frame().battle->completed_tick() == paused_tick,
            label + "agreed pause freezes child simulation and scripts");
        expect(live->frame().latest->clock().tactical_speed == double_speed, label + "tactical speed reaches both peers");
    }
    expect(feeds[0]->tick_hashes().size() == paused_hash_count && feeds[1]->tick_hashes().size() == paused_hash_count,
        label + "paused session ticks do not duplicate tactical history or effects");
    expect(scheduler->pending() == 0, label + "the real viewer scheduler drains while the child tick is frozen");
    const auto pending_input = scheduler->reservations();
    expect(pending_input.size() == 1 && !pending_input.front().execution_tick && acknowledged->load() != 1,
        label + "draining a paused intent does not acknowledge its execution");
    const auto agreed = feeds[0]->agreed_pacing();
    expect(agreed && agreed->paused && agreed->target_rate == 60,
        label + "presentation observes the agreed pause and rate without controlling them");
    feeds[1]->set_paused(false);
    expect(pair.run_to(60), label + "agreed resume releases the child");
    expect(!feeds[0]->paused() && !feeds[1]->paused()
        && pair.lives[0]->frame().battle->completed_tick() > paused_tick, label + "child resumes on both peers");
    expect(acknowledged->load() == 1 && execution_tick->load() >= paused_tick,
        label + "the authenticated producer receives its actual shared execution tick");
    scheduler->present_snapshot(feeds[1]->completed_tick());
    expect(scheduler->reservations().empty(), label + "presenting the executed child tick retires pending UI input");
    expect(pair.galaxy_frame(0) == frozen_galaxy && pair.galaxy_frame(1) == frozen_galaxy,
        label + "no galaxy frame advances while the child runs");
    expect(!pair.lives[0]->battle_is_ready(conflict) && !pair.lives[1]->battle_is_ready(conflict),
        label + "no local viewer-ready signal gates the shared child");
    const auto defender = std::find_if(frame.conflict->forces.begin(), frame.conflict->forces.end(),
        [](const auto& row) { return row.owner == 2; })->id;
    // Engine effects are refused even without seat_control; an authenticated attack settles the battle.
    feeds[0]->submit({1, {defender}, tactical::DamagePayload{math::Fixed::from_integer(1000).value()}, {}});
    feeds[0]->submit({1, {own}, tactical::AttackPayload{defender}, {}});
    std::optional<std::uint64_t> returned;
    for (std::uint64_t tick = 61; tick <= 85; ++tick) {
        expect(pair.run_to(tick), label + "battle and return ticks complete");
        const auto a = pair.lives[0]->frame(); const auto b = pair.lives[1]->frame();
        expect(bool(a.conflict) == bool(b.conflict), label + "both views return at the agreed tick");
        if (!returned && !a.conflict) {
            returned = tick;
            expect(a.result && b.result && a.result == b.result && a.result->conflict == conflict,
                label + "both peers reconcile the same result exactly at return");
        }
    }
    if (!returned) {
        const auto last = pair.lives[0]->frame();
        std::cerr << "child still active at " << last.session_tick << ", tactical tick "
            << (last.battle ? last.battle->completed_tick() : 0) << ", tactical commands " << feeds[0]->record().commands.size()
            << ", latest combat events " << (last.battle ? last.battle->combat_events().size() : 0) << '\n';
        for (const auto& error : pair.lives[0]->rejected_orders()) std::cerr << error.code << ": " << error.message << '\n';
    }
    expect(returned.has_value(), label + "the child returns to the galaxy");
    for (const auto peer : {0U, 1U}) {
        const auto view = pair.lives[peer]->frame(pair.humans[peer]);
        expect(view.latest && view.latest->view_player() == peer + 1, label + "child return retains this peer's human view");
        if (view.latest) for (const auto& row : view.latest->observations())
            expect(row.player == peer + 1, label + "child return does not publish the other human's observer rows");
    }
    // An input can be validly framed and authenticated after its conflict closes. Execution refuses it identically.
    galactic::TacticalLayerCommand stale{conflict, 5, {}};
    pair.order(0, stale);
    expect(pair.run_to(95), label + "a closed-conflict order does not terminate the match");
    for (auto& live : pair.lives) live->stop();
    expect(pair.lives[0]->tick_hashes() == pair.lives[1]->tick_hashes(), label + "whole-game digests agree through Begin, battle and return");
    expect(!pair.lives[0]->failure() && !pair.lives[1]->failure(), label + "no lockstep failure");
    const auto rejected_a = pair.lives[0]->rejected_orders();
    const auto rejected_b = pair.lives[1]->rejected_orders();
    expect(rejected_a.size() == rejected_b.size() && !rejected_a.empty()
        && rejected_a.back().code == rejected_b.back().code && rejected_a.back().message == rejected_b.back().message,
        label + "closed-conflict input is refused identically at execution");
    expect(std::any_of(rejected_a.begin(), rejected_a.end(), [](const auto& row) {
        return row.message.find("engine-only") != std::string::npos;
    }), label + "a human cannot inject engine tactical effects in a policy-free multiplayer setup");
    expect(std::any_of(rejected_a.begin(), rejected_a.end(), [](const auto& row) {
        return row.message.find("invalid tactical command body") != std::string::npos;
    }), label + "a short nested producer key is refused safely without execution acknowledgement");
    const auto recorded = pair.lives[0]->record().commands;
    expect(std::count_if(recorded.begin(), recorded.end(), [](const auto& row) {
        return std::holds_alternative<galactic::TacticalLayerCommand>(row.payload);
    }) >= 4, label + "both seats and the paused order reached the tactical command stream");
    if (workers[0] == 1) for (const auto count : {1U, 2U, 4U, 8U}) {
        namespace checkpoint = eawr::checkpoint;
        auto oracle = require(eawr::campaign::GameSession::create(initial));
        const auto configured = services->configure(oracle);
        if (!configured) throw std::runtime_error(configured.error().message);
        const eawr::platform::ThreadWorkerAdapter executor(count);
        const eawr::platform::ThreadWorkerAdapter restore_executor(count == 1 ? 8 : 1);
        const auto identities = checkpoint::game_identities(initial);
        auto load = services->restore;
        load.script_requirement = eawr::campaign::ScriptRequirement::none;
        load.vote_progress = child_vote_progress();
        std::array<std::optional<checkpoint::GameRestore>, 2> restored;
        std::array<std::size_t, 2> restored_next{};
        std::vector<std::string> hashes;
        std::size_t next = 0;
        for (std::uint64_t tick = 0; tick < 95; ++tick) {
            while (next < recorded.size() && recorded[next].key.tick == tick) {
                const auto submitted = oracle.submit(recorded[next++]);
                if (!submitted) throw std::runtime_error(submitted.error().message);
            }
            const auto stepped = require(oracle.step(executor));
            hashes.push_back(stepped.state_sha256.empty() ? stepped.state_hash.get() : stepped.state_sha256);
            for (std::size_t saved_index = 0; saved_index < restored.size(); ++saved_index) if (restored[saved_index]) {
                while (restored_next[saved_index] < recorded.size() && recorded[restored_next[saved_index]].key.tick == tick) {
                    const auto submitted = restored[saved_index]->session.submit(recorded[restored_next[saved_index]++]);
                    if (!submitted) throw std::runtime_error(submitted.error().message);
                }
                const auto resumed = require(restored[saved_index]->session.step(restore_executor));
                expect(resumed.state_hash.get() == hashes.back(), label + "mid-pause restore continues with the same whole-game digest");
            }
            if (tick == 44 || tick == 45) {
                const auto saved_index = static_cast<std::size_t>(tick - 44);
                expect(oracle.child_paused(), label + "checkpoint is captured with an agreed paused child");
                const auto saved = require(checkpoint::save_game_checkpoint(oracle, identities));
                restored[saved_index].emplace(require(checkpoint::load_game_checkpoint(saved, identities, load, restore_executor)));
                restored_next[saved_index] = next;
                expect(restored[saved_index]->session.state_sha256() == oracle.state_sha256(), label + "saved multiplayer setup restores the paused digest");
                expect(require(checkpoint::save_game_checkpoint(restored[saved_index]->session, identities)) == saved,
                    label + "two consecutive paused saves each restore byte-identically on another worker count");
                const auto window = require(oracle.checkpoint_recording());
                expect(window.commands.commands.size() + window.commands.native_effects.size() <= galactic::galactic_max_commands
                    && require(galactic::write_replay(window.commands)).size() <= galactic::galactic_replay_max_bytes,
                    label + "retained paused history remains within the existing command and encoded byte caps");
            }
        }
        expect(hashes == pair.lives[0]->tick_hashes(), label + "the same stream reproduces every whole-game digest at "
            + std::to_string(count) + " workers");
        for (const auto& saved : restored) {
            expect(saved && saved->session.galaxy().canonical_state_bytes() == oracle.galaxy().canonical_state_bytes()
                && saved->session.battle_record() == oracle.battle_record(),
                label + "both restored galaxies and complete tactical recordings end equal");
            expect(saved && require(checkpoint::save_game_checkpoint(saved->session, identities))
                == require(checkpoint::save_game_checkpoint(oracle, identities)),
                label + "both paused restores end in byte-identical complete checkpoint state");
        }
        // Deferred commands retain their original stamps. Export restores canonical per-seat sequence order
        // even when a resume executed before an older held order physically reached the child.
        const auto journal = oracle.record().commands;
        expect(std::is_sorted(journal.begin(), journal.end(), [](const auto& a, const auto& b) { return a.key < b.key; }),
            label + "held input keeps canonical per-seat order in the executed journal");
        auto replay = require(eawr::campaign::GameSession::from_recording(oracle.recording(), load.content, {},
            eawr::campaign::ScriptRequirement::none));
        replay.set_vote_progress(child_vote_progress());
        for (std::size_t tick = 0; tick < hashes.size(); ++tick)
            expect(require(replay.step(executor)).state_hash.get() == hashes[tick],
                label + "recording replay derives pacing from saved setup without a caller flag");
    }
}

void child_and_strategic_clock_share_one_limit(const std::array<std::size_t, 2> workers) {
    const auto initial = multiplayer_child_setup();
    Pair pair(initial, workers, LiveGameSession::Pacing::driven, 3000, 0);
    const auto services = child_services(initial, false);
    const auto configure = [services](LiveGameSession::Options& options) { options.services = services; };
    pair.start(0, configure); pair.start(1, configure);
    expect(pair.run_to(10), "limited clock fixture reaches the pending conflict");
    const auto pending = pair.lives[0]->frame().pending_conflict;
    if (!pending) throw std::runtime_error("missing conflict in shared clock budget fixture");
    const auto conflict = pending->conflict.id;
    pair.order(0, galactic::SetSpeed{false, math::Fixed::from_raw(math::Fixed::scale / 2)});
    expect(pair.run_to(15), "strategic speed uses the first shared budget slot");
    expect(pair.speed(0).raw() == math::Fixed::scale / 2 && pair.speed(1).raw() == math::Fixed::scale / 2,
        "strategic speed reaches both peers before the child begins");
    for (net::PeerIndex peer = 0; peer < 2; ++peer) pair.order(peer, galactic::BattleChoice{conflict, true});
    expect(pair.run_to(25), "limited clock fixture opens the child");
    std::array<std::shared_ptr<eawr::platform::TacticalFeed>, 2> feeds;
    for (net::PeerIndex peer = 0; peer < 2; ++peer) feeds[peer] = pair.lives[peer]->battle_feed(conflict, pair.humans[peer]);
    if (!feeds[0] || !feeds[1]) throw std::runtime_error("missing child feed in shared clock budget fixture");
    feeds[0]->set_paused(true);
    expect(pair.run_to(35) && feeds[0]->paused() && feeds[1]->paused(), "child pause uses the second shared budget slot");
    const auto child_tick = pair.lives[0]->frame().battle->completed_tick();
    feeds[0]->set_target_rate(60); // Third clock request from this seat, across the layer boundary.
    expect(pair.run_to(45), "rate refusal keeps the shared session running while paused");
    for (net::PeerIndex peer = 0; peer < 2; ++peer) {
        const auto frame = pair.lives[peer]->frame();
        expect(frame.latest->clock().tactical_speed.raw() == math::Fixed::scale,
            "third clock request cannot change tactical speed on either peer");
        expect(frame.battle->completed_tick() == child_tick, "a refused clock change cannot advance the paused child");
        const auto errors = pair.lives[peer]->rejected_orders();
        expect(std::count_if(errors.begin(), errors.end(), [](const auto& error) {
            return error.message.find("Shared clock limit") != std::string::npos;
        }) == 1, "both peers publish exactly one identical clock refusal");
        const auto view = pair.matches[peer]->view();
        expect(view.clock_refusals.size() == 1 && view.clock_refusals.front().seat == pair.humans[0],
            "HUD keeps a per-seat clock refusal receipt across skipped frames");
    }
    feeds[0]->set_paused(false);
    expect(pair.run_to(55) && !feeds[0]->paused() && !feeds[1]->paused(), "resume is always allowed despite an exhausted budget");
    feeds[0]->set_paused(true); // Refused pause leaves no permanent local pending request.
    expect(pair.run_to(65) && !feeds[0]->paused() && !feeds[1]->paused(), "an over-budget pause is refused");
    expect(pair.run_to(1400), "the shared clock window expires while session ticks continue");
    feeds[0]->set_paused(true); // Exactly the same desired value as the earlier refused request.
    expect(pair.run_to(1410) && feeds[0]->paused() && feeds[1]->paused(), "a refused child pause can be retried after expiry");
    feeds[1]->set_paused(false);
    expect(pair.run_to(1420) && !feeds[0]->paused() && !feeds[1]->paused(), "another human may always resume");
    feeds[0]->set_target_rate(60);
    expect(pair.run_to(1430), "the expired window admits another speed change");
    for (auto& live : pair.lives) live->stop();
    expect(pair.lives[0]->frame().latest->clock().tactical_speed == double_speed
        && pair.lives[1]->frame().latest->clock().tactical_speed == double_speed,
        "a permitted tactical speed applies equally to both peers");
    expect(pair.lives[0]->tick_hashes() == pair.lives[1]->tick_hashes(),
        "strategic/child clock budget acceptance and refusal keep equal digests at every worker count");
}

void auto_resolve_uses_the_agreed_step(const std::array<std::size_t, 2> workers) {
    const auto initial = multiplayer_child_setup();
    Pair pair(initial, workers, LiveGameSession::Pacing::driven, 3000, 0);
    const auto services = child_services(initial, true);
    const auto configure = [services](LiveGameSession::Options& options) { options.services = services; };
    pair.start(0, configure); pair.start(1, configure);
    expect(pair.run_to(20), "auto-resolve completes inside the shared step");
    const auto a = pair.lives[0]->frame(); const auto b = pair.lives[1]->frame();
    expect(a.result && b.result && a.result == b.result && a.result->winner == 2,
        "both peers publish the same auto-resolved land result");
    expect(!a.conflict && !b.conflict && !a.battle && !b.battle, "land auto-resolve never creates a tactical child");
    for (auto& live : pair.lives) live->stop();
    expect(pair.lives[0]->tick_hashes() == pair.lives[1]->tick_hashes(), "auto-resolve whole-game tick digests agree");
    static std::optional<std::vector<std::string>> reference_hashes;
    if (!reference_hashes) reference_hashes = pair.lives[0]->tick_hashes();
    else expect(pair.lives[0]->tick_hashes() == *reference_hashes, "auto-resolve digests equal the 1-worker run");
}

void child_requires_saved_multiplayer_setup() {
    const auto initial = versus_setup();
    Pair pair(initial, {1, 1}, LiveGameSession::Pacing::driven, 3000, 0);
    const auto services = child_services(initial, false);
    const auto configure = [services](LiveGameSession::Options& options) { options.services = services; };
    pair.start(0, configure); pair.start(1, configure);
    expect(pair.run_to(10), "a galaxy-only setup can still drive the galaxy");
    const auto pending = pair.lives[0]->frame().pending_conflict;
    if (!pending) throw std::runtime_error("missing pending conflict for setup refusal");
    for (net::PeerIndex peer = 0; peer < 2; ++peer)
        pair.order(peer, galactic::BattleChoice{pending->conflict.id, true});
    for (auto& live : pair.lives) live->advance_to(30);
    expect(until([&] { return pair.lives[0]->failure() && pair.lives[1]->failure(); }),
        "a child without persisted multiplayer mode fails on both peers");
    for (auto& live : pair.lives) {
        const auto failure = live->failure();
        expect(failure && failure->message.find("saved multiplayer") != std::string::npos,
            "the refusal names the missing saved setup mode");
    }
}

void conflicting_options_are_refused() {
    namespace cp = eawr::campaign;
    const auto initial = versus_setup();
    Pipes pipes(2);
    PipeTransport transport(pipes, 0);
    viewer::GalacticLanOptions lan;
    lan.seats = {{human_seats(initial)[0], true}, {human_seats(initial)[1], true}};
    viewer::GalacticLanMatch match(transport, lan);
    LiveGameSession::Options options;
    options.step_driver = match.driver();
    cp::ConflictRecord entry;
    entry.id = 77;
    options.entries.push_back({3, entry, {}, cp::ScriptRequirement::none});
    const auto started = LiveGameSession::start(initial, std::move(options));
    expect(!started && started.error().code == "EAWR-PLATFORM-0600", "local conflict entries cannot combine with a lockstep driver");
}

void presentation_barrier_is_refused() {
    Pair pair(versus_setup(), {1, 1}, LiveGameSession::Pacing::driven, 3000, 0);
    LiveGameSession::Options options;
    options.step_driver = pair.matches[0]->driver();
    options.await_presentation = true;
    const auto started = LiveGameSession::start(pair.setup, std::move(options));
    expect(!started && started.error().code == "EAWR-PLATFORM-0600"
        && started.error().message.find("presentation") != std::string::npos,
        "a lockstep presentation barrier is refused before starting a thread");
}

void saves_are_refused() {
    Pair pair(versus_setup(), {1, 1});
    expect(pair.run_to(5), "save refusal fixture runs");
    eawr::platform::LiveSaveRequest request;
    request.seat = pair.humans[0]; request.name = "lockstep-save";
    const auto token = pair.lives[0]->request_save(std::move(request));
    const auto outcomes = pair.lives[0]->save_outcomes();
    expect(token != 0 && outcomes.size() == 1 && outcomes.front().token == token
        && outcomes.front().phase == eawr::platform::LiveSavePhase::rejected
        && outcomes.front().diagnostic && outcomes.front().diagnostic->code == "EAWR-PLATFORM-0600"
        && outcomes.front().diagnostic->message.find("GCMP-17, #4369") != std::string::npos,
        "lockstep save refusal names the agreed checkpoint follow-up");
    expect(pair.run_to(10) && pair.lives[0]->record().commands.empty(),
        "a refused save queues no input and leaves the match running");
}

void driver_cannot_outlive_its_session() {
    Pair pair(versus_setup(), {1, 1});
    expect(pair.run_to(5), "driver lifetime fixture runs");
    for (auto& live : pair.lives) live->stop();
    pair.lives[0].reset(); // the adapter's original session and executor no longer exist
    pair.start(0);
    pair.lives[0]->advance_to(10);
    expect(until([&] { return pair.lives[0]->failure().has_value(); }, 5s), "a reused driver refuses to drive");
    const auto failure = pair.lives[0]->failure();
    expect(failure && failure->code == "EAWR-PLATFORM-0600"
        && failure->message.find("session ended") != std::string::npos
        && pair.lives[0]->completed_tick() == 0, "reuse fails before touching the old session references");
}

void completed_match_idles_until_session_end() {
    constexpr std::uint64_t final_tick = 20;
    Pair pair(versus_setup(), {1, 4}, LiveGameSession::Pacing::driven, 3000, 2, final_tick);
    expect(pair.run_to(final_tick), "both peers reach the final tick");
    expect(until([&] {
        return pair.matches[0]->view().reason == viewer::GalacticLanReason::completed
            && pair.matches[1]->view().reason == viewer::GalacticLanReason::completed;
    }, 5s), "both peers agree on normal completion");
    // An order forces another drive after completion and provides an observable idle-path acknowledgement.
    for (net::PeerIndex peer = 0; peer < 2; ++peer) pair.order(peer, galactic::PauseGalaxy{});
    expect(until([&] {
        return !pair.lives[0]->rejected_orders().empty() && !pair.lives[1]->rejected_orders().empty();
    }, 5s), "completed matches reject further orders while still servicing the driver");
    for (auto& live : pair.lives) live->stop();
    for (const auto& live : pair.lives)
        expect(!live->failure() && live->completed_tick() == final_tick,
            "normal completion idles successfully without another tick");
    pair.lives[0].reset();
    pair.start(0);
    pair.lives[0]->advance_to(1);
    expect(until([&] { return pair.lives[0]->failure().has_value(); }, 5s),
        "a completed match driver cannot be reused after its live session ends");
    const auto failure = pair.lives[0]->failure();
    expect(failure && failure->message.find("session ended") != std::string::npos
        && pair.lives[0]->completed_tick() == 0, "completed driver reuse fails before stepping");
}

void acceptance_recorder_observes_without_changing_ticks() {
    const auto directory = std::filesystem::temp_directory_path() / ("eawr-gc-lan-acceptance-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    constexpr std::uint64_t final_tick = 20;
    Pair recorded(versus_setup(), {1, 4}, LiveGameSession::Pacing::driven, 3000, 0, final_tick);
    for (net::PeerIndex peer = 0; peer < 2; ++peer) {
        recorded.matches[peer]->configure_acceptance(directory / std::to_string(peer), "synthetic-contract", false);
        recorded.start(peer);
    }
    expect(recorded.run_to(final_tick), "recorded peers reach the ordinary final tick");
    expect(until([&] {
        return recorded.matches[0]->view().reason == viewer::GalacticLanReason::completed
            && recorded.matches[1]->view().reason == viewer::GalacticLanReason::completed;
    }, 5s), "qualification retains the ordinary final-hash barrier");
    for (auto& live : recorded.lives) live->stop();
    Pair ordinary(versus_setup(), {1, 4}, LiveGameSession::Pacing::driven, 3000, 2, final_tick);
    expect(ordinary.run_to(final_tick), "unrecorded reference reaches the same tick");
    for (auto& live : ordinary.lives) live->stop();
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream stream(path);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };
    expect(recorded.lives[0]->tick_hashes() == ordinary.lives[0]->tick_hashes(),
        "read-only recording preserves every hooks-off digest before scripted orders");
    expect(recorded.lives[0]->record() == ordinary.lives[0]->record(), "read-only recording preserves replay bytes");
    const auto hashes = read(directory / "0/hashes.csv");
    expect(hashes == read(directory / "1/hashes.csv"), "recorder writes equal complete peer hash streams");
    expect(static_cast<std::uint64_t>(std::count(hashes.begin(), hashes.end(), '\n')) == final_tick + 1,
        "recorder writes one row per tick plus header");
    expect(read(directory / "0/gc.json").find("\"agreed_end\":true") != std::string::npos,
        "receipt proves the agreed barrier, rather than a forced stop");
    const auto costs = read(directory / "0/costs.csv");
    expect(static_cast<std::uint64_t>(std::count(costs.begin(), costs.end(), '\n')) == final_tick + 1,
        "costs cover every completed tick");
    std::filesystem::remove_all(directory);
}

void acceptance_script_changes_each_humans_speed() {
    const auto directory = std::filesystem::temp_directory_path() / ("eawr-gc-lan-clock-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    constexpr std::uint64_t final_tick = 2000;
    Pair pair(versus_setup(), {1, 4}, LiveGameSession::Pacing::driven, 3000, 0, final_tick);
    for (net::PeerIndex peer = 0; peer < 2; ++peer) {
        pair.matches[peer]->configure_acceptance(directory / std::to_string(peer), "synthetic-contract", false);
        pair.start(peer);
    }
    for (auto& live : pair.lives) live->advance_to(final_tick);
    expect(until([&] {
        return pair.matches[0]->view().reason == viewer::GalacticLanReason::completed
            && pair.matches[1]->view().reason == viewer::GalacticLanReason::completed;
    }, 120s), "scripted shared clock reaches the agreed final barrier within its tick-scaled deadline");
    for (auto& live : pair.lives) live->stop();
    std::ifstream stream(directory / "0/gc.json");
    const std::string receipt{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    expect(receipt.find("\"barrier_tick\":2000,") != std::string::npos,
        "clock-script receipt covers all requested session ticks");
    expect(pair.lives[0]->tick_hashes().size() == final_tick
        && pair.lives[0]->tick_hashes() == pair.lives[1]->tick_hashes(),
        "scripted clock peers complete every tick with equal digests");
    const auto changes = receipt.substr(receipt.find("\"clock_changes\":"));
    for (const auto seat : pair.humans) {
        bool changed = false;
        const auto prefix = "{\"seat\":" + std::to_string(seat.value) + ",";
        for (auto at = changes.find(prefix); at != std::string::npos; at = changes.find(prefix, at + 1)) {
            const auto row = changes.substr(at, changes.find('}', at) - at);
            changed = changed || row.find("\"kind\":\"speed\"") != std::string::npos;
        }
        expect(changed, "each human publishes an actual speed change, including the second human");
    }
    expect(receipt.find("EAWR-NET") == std::string::npos, "clock script respects LAN admission limits");
    stream.close();
    std::filesystem::remove_all(directory);
}

void desync_remains_a_failure() {
    Pair pair(versus_setup(), {1, 4}, LiveGameSession::Pacing::driven, 3000, 0);
    // A deliberately inconsistent engine source changes only one peer after setup agreement.
    pair.start(0, [&](LiveGameSession::Options& options) {
        options.command_source = [&](const std::uint64_t tick) {
            std::vector<galactic::GameCommand> commands;
            if (tick == 5) commands.push_back({{tick, {3}, 9000},
                galactic::DebugSetPlanetOwner{pair.setup.planets[5].id, 3}});
            return commands;
        };
    });
    pair.start(1);
    for (auto& live : pair.lives) live->advance_to(30);
    expect(until([&] { return pair.lives[0]->failure() && pair.lives[1]->failure(); }, 10s),
        "both live sessions fail when their match states diverge");
    for (net::PeerIndex peer = 0; peer < 2; ++peer) {
        const auto view = pair.matches[peer]->view();
        const auto failure = pair.lives[peer]->failure();
        expect(view.phase == viewer::GalacticLanPhase::ended && view.reason == viewer::GalacticLanReason::desync,
            "the ended match identifies desync");
        expect(failure && failure->code == "EAWR-PLATFORM-0600" && failure->message == view.message
            && failure->message.find("diverged") != std::string::npos,
            "the failure preserves the desync explanation");
    }
}

void engine_commands_run_on_each_peer() {
    std::array<std::vector<std::uint64_t>, 2> calls;
    Pair pair(versus_setup(), {1, 4}, LiveGameSession::Pacing::driven, 3000, 0);
    for (net::PeerIndex peer = 0; peer < 2; ++peer) {
        pair.start(peer, [&, peer](LiveGameSession::Options& options) {
            options.command_source = [&, peer](const std::uint64_t tick) {
                calls[peer].push_back(tick);
                std::vector<galactic::GameCommand> commands;
                if (tick == 5) commands.push_back({{tick, {3}, 9000}, galactic::DebugSetPlanetOwner{pair.setup.planets[5].id, 3}});
                return commands;
            };
        });
    }
    expect(pair.run_to(20), "engine command sources run under lockstep");
    for (auto& live : pair.lives) live->stop();
    expect(calls[0].size() == 20 && calls[0] == calls[1], "each peer sources exactly the executed ticks");
    expect(pair.lives[0]->record().commands.size() == 1 && pair.lives[1]->record().commands.size() == 1
        && pair.lives[0]->tick_hashes() == pair.lives[1]->tick_hashes(), "engine commands apply equally on both peers");
}

// Co-op over two live peers (owner update 10-21): both humans share faction 1 against the AI faction 2. The side is human,
// so the conflict opens the same Begin / Auto-Resolve vote on both peers and holds the strategic clock; one teammate's Begin
// opens the tactical child with both seats bound to the one tactical player; the campaign resumes when both are back.
void multiplayer_coop_battle() {
    auto initial = multiplayer_child_setup(3);
    for (auto& seat : initial.seats) {
        if (seat.seat_id.value == 1 || seat.seat_id.value == 2) { seat.player = 1; seat.human_side_id = 1; }
        else if (seat.seat_id.value == 3) seat.player = 2;
    }
    initial.observer->human_observers = {1};
    for (auto& player : initial.observer->players) player.ai = player.player != 1;
    for (auto& player : initial.victory->context.players) { player.human = player.player == 1; player.ai = !player.human; }
    initial.seat_control.emplace();
    initial.seat_control->teams = {{1, {1}, false}};
    for (const auto& player : initial.players)
        initial.seat_control->controllers.push_back({player.player_id,
            player.player_id == 1 ? galactic::FactionControl::human : galactic::FactionControl::ai, galactic::FactionControl::ai, {}, {}});
    Pair pair(initial, {1, 4}, LiveGameSession::Pacing::driven, 3000, 0);
    const auto services = child_services(initial, false, true, 450, true);
    const auto configure = [services](auto& options) { options.services = services; };
    pair.start(0, configure); pair.start(1, configure);
    expect(pair.run_to(10), "co-op reaches the conflict boundary");
    const auto pending = pair.lives[0]->frame().pending_conflict;
    expect(pending && pair.lives[1]->frame().pending_conflict && pending->arbitration.route == eawr::campaign::ConflictRoute::vote
        && !pair.lives[0]->frame().battle && !pair.lives[1]->frame().battle,
        "co-op side versus AI opens the Begin / Auto-Resolve vote on both peers, not a forced child");
    if (!pending) return;
    const auto before = pair.galaxy_frame(0);
    for (const auto peer : {0U, 1U}) {
        const auto view = pair.lives[peer]->frame(pair.humans[peer]).pending_conflict;
        expect(view && view->alert && view->alert->visible && view->alert->begin_admitted && view->alert->resolve_admitted
            && !view->alert->waiting_for_opponent, "each teammate sees the full prompt, not an opponent wait");
    }
    expect(pair.run_to(20) && pair.lives[0]->frame().pending_conflict && pair.galaxy_frame(0) == before
        && pair.galaxy_frame(1) == before, "the vote holds the campaign clock on both peers");
    pair.order(0, galactic::BattleChoice{pending->conflict.id, true});
    expect(pair.run_to(30), "the first teammate's Begin settles the side's vote");
    for (const auto peer : {0U, 1U}) {
        const auto frame = pair.lives[peer]->frame(pair.humans[peer]);
        expect(frame.battle && frame.conflict && !frame.pending_conflict, "Begin launches the tactical child on both peers");
        if (!frame.conflict) continue;
        std::size_t bound{};
        for (const auto& binding : frame.conflict->seats) bound += binding.player == 1 ? 1 : 0;
        expect(bound == 2, "both teammates' seats are bound to the co-op tactical player");
    }
    for (std::uint64_t tick = 31; tick <= 100; ++tick) {
        expect(pair.run_to(tick), "co-op child advances through agreed ticks");
        if (pair.lives[0]->frame().conflict) expect(pair.galaxy_frame(0) == before && pair.galaxy_frame(1) == before,
            "the campaign does not advance while the co-op battle runs");
    }
    expect(!pair.lives[0]->frame().conflict && !pair.lives[1]->frame().conflict, "both peers return from the co-op battle");
    expect(pair.run_to(120) && pair.galaxy_frame(0) > before && pair.galaxy_frame(0) == pair.galaxy_frame(1),
        "the campaign resumes equally after both teammates return");
    for (auto& live : pair.lives) live->stop();
    expect(!pair.lives[0]->failure() && !pair.lives[1]->failure(), "co-op battle never fails the session");
    expect(pair.lives[0]->tick_hashes() == pair.lives[1]->tick_hashes(), "co-op battle has equal digests every tick");
}

// The lone human of a human-versus-AI vote leaves before choosing: no human side remains, so the same live seat policy that
// binds the vote turns the conflict into an instant auto-resolve for the survivor, without a prompt or a tactical child.
void multiplayer_ai_vote_human_leaves() {
    auto initial = multiplayer_child_setup(3);
    for (auto& seat : initial.seats) {
        if (seat.seat_id.value == 2) seat.player = 3;
        else if (seat.seat_id.value == 3) seat.player = 2;
    }
    initial.observer->human_observers = {1, 3};
    for (auto& player : initial.observer->players) player.ai = player.player == 2;
    for (auto& player : initial.victory->context.players) { player.human = player.player != 2; player.ai = !player.human; }
    initial.seat_control.emplace();
    initial.seat_control->teams = {{1, {1}, false}, {2, {2}, false}};
    for (const auto& player : initial.players)
        initial.seat_control->controllers.push_back({player.player_id,
            player.player_id != 2 ? galactic::FactionControl::human : galactic::FactionControl::ai, galactic::FactionControl::ai, {}, {}});
    Pair pair(initial, {1, 4}, LiveGameSession::Pacing::driven, 3000, 0);
    const auto services = child_services(initial, false, true, 450, true);
    const auto configure = [services](auto& options) { options.services = services; };
    pair.start(0, configure); pair.start(1, configure);
    expect(pair.run_to(10), "the human-versus-AI vote is reached");
    const auto pending = pair.lives[0]->frame().pending_conflict;
    expect(pending && pair.lives[1]->frame().pending_conflict && pending->arbitration.route == eawr::campaign::ConflictRoute::vote,
        "the lone human's conflict against the AI waits in the vote on both peers");
    if (!pending) return;
    const auto frozen = pair.galaxy_frame(1);
    pair.order(0, galactic::SeatLeave{});
    for (auto& live : pair.lives) live->advance_to(32);
    expect(pair.lives[1]->wait_for(32, 30s), "the survivor advances through the agreed departure");
    expect(until([&] { return pair.matches[0]->view().reason == viewer::GalacticLanReason::local_left; }),
        "the departing peer completes the same transition frontier before stopping");
    const auto frame = pair.lives[1]->frame();
    expect(!frame.pending_conflict && !frame.battle && !frame.conflict && frame.auto_resolve_outcome,
        "the departure auto-resolves the conflict at once instead of opening a vote or a child");
    expect(!pair.lives[1]->frame(pair.humans[1]).auto_resolve_outcome, "WAR-68 the uninvolved survivor gets no result notice");
    pair.lives[1]->advance_to(60);
    expect(pair.lives[1]->wait_for(60, 30s) && pair.galaxy_frame(1) > frozen, "the campaign clock resumes after the instant result");
    for (auto& live : pair.lives) live->stop();
    expect(!pair.lives[0]->failure() && !pair.lives[1]->failure(), "a mid-vote departure never aborts a peer");
    const auto first = pair.lives[0]->tick_hashes(), second = pair.lives[1]->tick_hashes();
    expect(!first.empty() && second.size() >= first.size()
        && std::equal(first.begin(), first.end(), second.begin()),
        "every shared digest matches through the departure and the instant result");
    const auto commands = pair.lives[1]->record().commands;
    expect(std::none_of(commands.begin(), commands.end(), [](const auto& command) {
        return std::holds_alternative<galactic::BattleChoice>(command.payload);
    }), "no vote was ever cast");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--arbitration") {
            multiplayer_arbitration();
            multiplayer_vote_departure();
            multiplayer_coop_side_fights_the_ai();
            solo_keeps_every_auto_resolve_notice();
            return galactic_test::checker.finish();
        }
        if (argc == 2 && std::string_view(argv[1]) == "--coop") {
            multiplayer_coop_battle();
            multiplayer_ai_vote_human_leaves();
            return galactic_test::checker.finish();
        }
        if (argc == 2 && std::string_view(argv[1]) == "--raid") {
            raid_commands_and_digests();
            return galactic_test::checker.finish();
        }
        raid_commands_and_digests();
        const std::array<std::array<std::size_t, 2>, 4> combinations{{{1, 8}, {2, 4}, {4, 1}, {8, 2}}};
        for (const auto& workers : combinations) {
            agreed_commands_and_digests(workers);
            private_views_and_digests(workers);
            invasion_accept_is_agreed(workers);
        }
        real_time_follows_the_agreed_clock();
        refusals_never_stamp_locally();
        views_connect_wait_and_run();
        departure_continues_alone();
        closed_link_applies_fallback();
        for (const auto& workers : combinations) {
            child_battle_uses_the_agreed_frontier(workers);
            child_and_strategic_clock_share_one_limit(workers);
            auto_resolve_uses_the_agreed_step(workers);
        }
        child_requires_saved_multiplayer_setup();
        conflicting_options_are_refused();
        presentation_barrier_is_refused();
        saves_are_refused();
        driver_cannot_outlive_its_session();
        completed_match_idles_until_session_end();
        acceptance_recorder_observes_without_changing_ticks();
        acceptance_script_changes_each_humans_speed();
        desync_remains_a_failure();
        engine_commands_run_on_each_peer();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}
