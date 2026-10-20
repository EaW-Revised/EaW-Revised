#include "../galactic/galactic_support.hpp"
#include "galactic_lan_match.hpp"

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/synthetic.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
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

galactic::GalacticSetup versus_setup() {
    eawr::campaign::SyntheticCampaign campaign;
    campaign.name = "net-galactic-live-versus";
    campaign.galaxy.seed = 31;
    campaign.galaxy.planets = 24;
    campaign.galaxy.players = 3;
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
        const std::uint64_t final_tick_count = net::max_session_ticks)
        : setup(std::move(initial)), humans(human_seats(setup)), pipes(2), worker_counts(workers), pacing_(pacing) {
        if (humans.size() != 2) throw std::runtime_error("the fixture needs exactly two human seats");
        viewer::GalacticLanOptions options;
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

// The unnetworked oracle: the same stamped commands in one GameSession.
std::vector<std::string> reference(const galactic::GalacticSetup& setup, std::vector<galactic::GameCommand> commands,
    const std::uint64_t ticks) {
    std::sort(commands.begin(), commands.end(), [](const auto& left, const auto& right) { return left.key < right.key; });
    auto game = require(eawr::campaign::GameSession::create(setup));
    for (const auto& command : commands) require(game.submit(command));
    const eawr::platform::ThreadWorkerAdapter executor(1);
    std::vector<std::string> hashes;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = require(game.step(executor));
        hashes.push_back(stepped.state_sha256.empty() ? stepped.state_hash.get() : stepped.state_sha256);
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
    const auto frozen = pair.galaxy_frame(0);
    expect(pair.run_to(50), label + "the paused session still ticks");
    expect(pair.galaxy_frame(0) == frozen && pair.galaxy_frame(1) == frozen, label + "the paused galaxy frame does not move");
    pair.order(0, galactic::ResumeGalaxy{});
    pair.order(1, galactic::SetSpeed{false, double_speed});
    expect(pair.run_to(80), label + "resume and speed change are agreed commands");
    expect(pair.pause(0) == 0 && pair.pause(1) == 0, label + "the agreed resume reached both peers");
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

void real_time_follows_the_agreed_clock() {
    Pair pair(versus_setup(), {2, 2}, LiveGameSession::Pacing::real_time);
    pair.order(0, galactic::SetSpeed{false, double_speed});
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

// A child battle opened under lockstep must fail loudly instead of running the local battle pacer (GCMP-05 owns the barrier).
void child_battle_fails_loudly() {
    namespace cp = eawr::campaign;
    namespace tactical = eawr::sim::tactical;
    const auto initial = versus_setup();
    Pair pair(initial, {1, 1}, LiveGameSession::Pacing::driven, 3000, 0);
    auto stage = [&](LiveGameSession::Options& options) {
        auto services = std::make_shared<eawr::platform::CampaignServiceConfiguration>();
        services->configure = [initial](cp::GameSession& game) {
            cp::ConflictRecord entry;
            entry.id = game.galaxy().next_entity_id();
            entry.planet = initial.planets.front().id; entry.invader = 1; entry.defender = 2;
            entry.tactical.content_identity = initial.content_identity; entry.tactical.first_free_entity_id = entry.id + 1;
            entry.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
            tactical::TacticalContent content;
            for (const auto owner : {1U, 2U}) {
                const auto fleet = std::find_if(initial.fleets.begin(), initial.fleets.end(), [owner](const auto& row) { return row.owner == owner; });
                const auto member = fleet->members.front();
                entry.forces.push_back({member.id, member.type, owner, fleet->id});
                tactical::UnitState unit; unit.entity_id = member.id; unit.type_id = member.type; unit.owner = owner;
                entry.tactical.units.push_back(unit);
            }
            for (const auto& seat : initial.seats) if (seat.player <= 2) entry.seats.push_back({seat.seat_id, seat.player});
            tactical::DurabilityProfile hull; hull.type_id = entry.forces.front().type; hull.max_hull = math::Fixed::from_integer(100).value();
            content.durability.profiles.push_back(hull);
            content.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
            content.victory.contenders = {1, 2}; content.victory.controlled_players = {1, 2}; content.victory.humans = {1};
            content.victory.countdown_frames = 2; content.victory.relevant_types = {hull.type_id};
            return game.stage_battle(entry, content, cp::ScriptRequirement::none);
        };
        options.services = std::move(services);
    };
    pair.start(0, stage);
    pair.start(1, stage);
    for (auto& live : pair.lives) live->advance_to(40);
    expect(until([&] { return pair.lives[0]->failure() && pair.lives[1]->failure(); }, 20s), "both peers fail when a child battle opens");
    for (auto& live : pair.lives) {
        const auto failure = live->failure();
        expect(failure && failure->code == "EAWR-PLATFORM-0600" && failure->message.find("child battle") != std::string::npos,
            "the failure names the child battle");
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

} // namespace

int main() {
    try {
        const std::array<std::array<std::size_t, 2>, 4> combinations{{{1, 8}, {2, 4}, {4, 1}, {8, 2}}};
        for (const auto& workers : combinations) {
            agreed_commands_and_digests(workers);
            invasion_accept_is_agreed(workers);
        }
        real_time_follows_the_agreed_clock();
        refusals_never_stamp_locally();
        views_connect_wait_and_run();
        departure_continues_alone();
        closed_link_applies_fallback();
        child_battle_fails_loudly();
        conflicting_options_are_refused();
        presentation_barrier_is_refused();
        saves_are_refused();
        driver_cannot_outlive_its_session();
        completed_match_idles_until_session_end();
        desync_remains_a_failure();
        engine_commands_run_on_each_peer();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}
