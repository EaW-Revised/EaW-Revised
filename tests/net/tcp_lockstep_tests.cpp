#include "eawr/net/tcp_transport.hpp"
#if defined(EAWR_TEST_GNS)
#include "eawr/net/gns_transport.hpp"
#endif
#include "eawr/net/desync_monitor.hpp"
#include "eawr/net/tactical_adapter.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
namespace net = eawr::net;
namespace sim = eawr::sim;
namespace tactical = sim::tactical;
using sim::math::Fixed;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
#if defined(EAWR_TEST_GNS)
using TestTransport = net::GnsTransport;
using TestOptions = net::GnsOptions;
constexpr auto transport_timeout_code = "EAWR-NET-1507";
#else
using TestTransport = net::TcpTransport;
using TestOptions = net::TcpOptions;
constexpr auto transport_timeout_code = "EAWR-NET-0207";
#endif
void expect(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
Fixed units(std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

enum class DepartureCase { none, closed, silent };
void battle(DepartureCase departure = DepartureCase::none) {
    const bool depart = departure != DepartureCase::none;
    TestOptions tcp;
    tcp.build_identity = "tcp-lockstep-test";
    // Both peers are pumped on this thread. Parallel CTest can deschedule it
    // past a short idle timeout even with loopback bytes ready to read. Leave
    // scheduling headroom without slowing successful battles or changing defaults.
    tcp.keepalive = 50ms; tcp.timeout = 10s;
    auto host = take(TestTransport::host("127.0.0.1", 0, tcp));
    auto client = take(TestTransport::connect("127.0.0.1", host->port(), tcp));
    const auto start = Clock::now();
    while (host->link(1) != net::LinkState::open || client->link(0) != net::LinkState::open) {
        (void)host->poll(); (void)client->poll();
        expect(Clock::now() - start < 10s, "TCP handshake deadline");
        std::this_thread::sleep_for(1ms);
    }
    tactical::TacticalSetup setup;
    setup.seed = 714;
    if (depart) setup.departure_policy = tactical::MultiplayerDeparturePolicy{{1, 2}};
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    for (std::uint32_t player = 1; player <= 2; ++player) {
        tactical::UnitState unit;
        unit.entity_id = player; unit.type_id = 1; unit.owner = player;
        unit.position = {units(player == 1 ? -50 : 50), {}, {}};
        unit.rotation = player == 1 ? sim::math::identity_quat() : sim::math::Quat{{}, {}, units(1), {}};
        setup.units.push_back(unit);
    }
    tactical::TacticalContent content;
    content.sensors = {{1, units(1000)}};
    content.motion.rules.arc_degrees = units(15);
    content.motion.rules.expansion_distance = units(300);
    content.motion.profiles = {{1, units(2), units(1), units(1), units(5), units(1), {}, {}}};
    tactical::WeaponProfile weapon;
    weapon.range = units(500); weapon.min_recharge_hundredths = 20; weapon.max_recharge_hundredths = 30;
    weapon.opportunity_when_targeting = true;
    tactical::CombatProfile profile;
    profile.type_id = 1; profile.category_bits = 1; profile.max_attack_distance = units(500); profile.weapons = {weapon};
    content.combat.profiles = {profile};
    const std::vector<tactical::SeatBinding> seats{{sim::SeatId{1}, 1, true, false}, {sim::SeatId{2}, 2, true, false}};
    eawr::platform::ThreadWorkerAdapter one(1), four(4);
    net::SessionOptions options;
    options.input_delay = 3; options.final_tick_count = 70;
    options.completeness_timeout_ms = 2000; // Still exercises bounded application-stall departure.
    options.closed_link_reason = [&](net::PeerIndex) {
        return host->diagnostic() && host->diagnostic()->code == transport_timeout_code
            ? net::DepartureReason::timeout : net::DepartureReason::disconnect;
    };
    net::DesyncOptions monitor_options;
    monitor_options.input_delay = options.input_delay;
    monitor_options.exchange_every = 2;
    net::DesyncMonitor first_monitor(*host, monitor_options), second_monitor(*client, monitor_options);
    auto first = take(net::TacticalLockstepSession::create(setup, content, *host, one, seats, options,
        depart ? &first_monitor : nullptr));
    auto second = take(net::TacticalLockstepSession::create(setup, content, *client, four, seats, options,
        depart ? &second_monitor : nullptr));
    std::array<net::TacticalLockstepSession*, 2> sessions{&first, &second};
    std::array<unsigned, 2> script{};
    std::array<std::vector<std::string>, 2> hashes;
    std::array<std::size_t, 2> shots{};
    bool waited = false;
    while (first.simulation().completed_tick() < 70 || second.simulation().completed_tick() < 70) {
        expect(Clock::now() - start < 30s, "TCP lockstep battle deadline");
        const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
        for (std::size_t peer = 0; peer < sessions.size(); ++peer) {
            auto& session = *sessions[peer];
            const auto owner = static_cast<std::uint32_t>(peer + 1);
            const auto opponent = owner == 1 ? 2U : 1U;
            const auto completed = session.simulation().completed_tick();
            if (script[peer] == 0) {
                const auto stamp = take(session.submit({{owner}, tactical::AttackPayload{opponent}}));
                expect(stamp.tick == 3, "Initial command preserves input delay"); ++script[peer];
            } else if (script[peer] == 1 && completed >= 15) {
                take(session.submit({{owner}, tactical::MovePayload{{units(owner == 1 ? -20 : 20), units(10), {}}}}));
                ++script[peer];
            } else if (script[peer] == 2 && completed >= 30) {
                take(session.submit({{owner}, tactical::AttackPayload{opponent}})); ++script[peer];
            }
            const auto result = take(session.pump(now));
            if (result.completed) {
                hashes[peer].push_back(result.completed->state_sha256);
                for (const auto& event : result.completed->snapshot->combat_events())
                    if (event.kind == tactical::CombatEventKind::weapon_fired) ++shots[peer];
            }
            waited = waited || session.waiting_for().has_value();
        }
        if (depart && first.simulation().completed_tick() >= 20 && second.simulation().completed_tick() >= 20) {
            if (departure == DepartureCase::closed)
                client.reset(); // Abrupt link loss mid-battle, without an ordered quit/close.
            const auto killed = Clock::now();
            bool reported = false;
            while (!reported && Clock::now() - killed < tcp.timeout) {
                const auto time = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
                if (departure == DepartureCase::silent) (void)client->poll(); // Keep TCP alive, stop game inputs/hashes.
                const auto pumped = first.pump(time);
                if (!pumped) {
                    expect(pumped.error().code == net::session_errors::stalled, "unconfirmed transport loss is typed connection loss");
                    expect(first.departure_outcomes().empty() && !first.simulation().outcome(), "unconfirmed close or application stall awards no winner");
                    if (departure == DepartureCase::silent)
                        expect(host->link(1) == net::LinkState::open && client->link(0) == net::LinkState::open,
                            "application loss remains distinct from transport keepalive closure");
                    reported = true;
                }
                std::this_thread::sleep_for(1ms);
            }
            expect(reported, "TCP departure outcome arrives within keepalive timeout");
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
    expect(hashes[0].size() == 70 && hashes[0] == hashes[1], "TCP peers agree at every completed tick on 1/4 workers");
    expect(first.simulation().state_sha256() == second.simulation().state_sha256(), "TCP final state agrees");
    expect(first.simulation().record() == second.simulation().record(), "TCP canonical replays agree");
    expect(first.simulation().record().commands.size() == 6, "Both peers' scripted commands applied exactly once");
    expect(waited, "Real TCP exercises lockstep completeness waiting");
    expect(shots[0] > 0 && shots[0] == shots[1], "Both TCP peers execute equal weapon fire");
}
// Real TCP/GNS continues its keepalives while the fixture drops selected application frames.
class GapTransport final : public net::MessageTransport {
public:
    explicit GapTransport(net::MessageTransport& transport, bool hash_gap) : transport_(transport), hash_gap_(hash_gap) {}
    bool gap{};
    eawr::core::Result<void> send(net::PeerIndex peer, std::span<const std::uint8_t> frame) override {
        return gap && drops(frame) ? eawr::core::Result<void>::success() : transport_.send(peer, frame);
    }
    std::optional<net::Received> poll() override {
        for (unsigned count = 0; count < 256; ++count) {
            auto next = transport_.poll();
            if (!next || !gap || !drops(next->frame)) return next;
        }
        return {};
    }
    net::LinkState link(net::PeerIndex peer) const override { return transport_.link(peer); }
    net::PeerIndex self() const override { return transport_.self(); }
    std::uint32_t peer_count() const override { return transport_.peer_count(); }
private:
    bool drops(std::span<const std::uint8_t> bytes) const {
        return !bytes.empty() && (hash_gap_ ? bytes.front() == net::desync_combined_kind || bytes.front() == net::desync_rows_kind
            : bytes.front() == net::sealed_batch_kind);
    }
    net::MessageTransport& transport_;
    bool hash_gap_{};
};
void mutual_loss_or_real_quit(unsigned workers, bool hash_gap, bool real_quit = false) {
    TestOptions options; options.build_identity = "mutual-loss-contract"; options.timeout = 10s;
    auto host = take(TestTransport::host("127.0.0.1", 0, options));
    auto client = take(TestTransport::connect("127.0.0.1", host->port(), options));
    const auto deadline = Clock::now() + 10s;
    while (host->link(1) != net::LinkState::open || client->link(0) != net::LinkState::open) {
        (void)host->poll(); (void)client->poll();
        expect(Clock::now() < deadline, "mutual-loss handshake deadline"); std::this_thread::sleep_for(1ms);
    }
    GapTransport left(*host, hash_gap), right(*client, hash_gap);
    tactical::TacticalSetup setup; setup.seed = 3794;
    setup.departure_policy = tactical::MultiplayerDeparturePolicy{{1, 2}};
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    for (std::uint32_t player = 1; player <= 2; ++player) {
        tactical::UnitState unit; unit.entity_id = player; unit.type_id = 1; unit.owner = player;
        unit.position = {units(player == 1 ? -50 : 50), {}, {}}; unit.rotation = sim::math::identity_quat(); setup.units.push_back(unit);
    }
    const std::vector<tactical::SeatBinding> seats{{sim::SeatId{1}, 1, true, false}, {sim::SeatId{2}, 2, true, false}};
    eawr::platform::ThreadWorkerAdapter executor(workers);
    net::SessionOptions policy; policy.final_tick_count = 1000; policy.completeness_timeout_ms = 30'000;
    net::DesyncOptions checks; checks.input_delay = policy.input_delay; checks.exchange_every = 2;
    net::DesyncMonitor left_monitor(left, checks), right_monitor(right, checks);
    auto first = take(net::TacticalLockstepSession::create(setup, {}, left, executor, seats, policy, hash_gap ? &left_monitor : nullptr));
    auto second = take(net::TacticalLockstepSession::create(setup, {}, right, executor, seats, policy, hash_gap ? &right_monitor : nullptr));
    std::array<net::TacticalLockstepSession*, 2> sessions{&first, &second};
    std::uint64_t now{};
    while (first.simulation().completed_tick() < 20 || second.simulation().completed_tick() < 20) {
        take(first.pump(now)); take(second.pump(now++));
        expect(Clock::now() < deadline, "mutual-loss warmup deadline"); std::this_thread::sleep_for(1ms);
    }
    if (real_quit) take(first.submit({{}, tactical::QuitPayload{}}));
    else left.gap = right.gap = true;
    const auto gap_start = now;
    std::array<std::uint64_t, 2> wait_start{gap_start, gap_start};
    for (unsigned peer = 0; peer < 2; ++peer)
        if (const auto& waiting = sessions[peer]->waiting_for()) wait_start[peer] = waiting->since_ms;
    std::array<bool, 2> lost{};
    while (!(real_quit ? first.simulation().outcome().has_value() && second.simulation().outcome().has_value() : lost[0] && lost[1])) {
        for (unsigned peer = 0; peer < 2; ++peer) if (!lost[peer]) {
            auto result = sessions[peer]->pump(now);
            if (!result) {
                expect(!real_quit && result.error().code == net::session_errors::stalled, "mutual receive gap is connection loss, never a quit");
                expect(now - wait_start[peer] >= 30'000, "loss does not fire before the required peer has been missing for 30 seconds");
                expect(result.error().message.starts_with("Connection lost: the match could not continue"), "both peers get explicit connection-loss cause");
                lost[peer] = true;
            }
        }
        now += real_quit ? 1U : 100U;
        expect(Clock::now() < deadline && now - gap_start < 40'000, "both loss decisions remain bounded");
        std::this_thread::sleep_for(1ms);
    }
    if (real_quit) {
        for (auto* session : sessions)
            expect(session->simulation().outcome()->condition == tactical::VictoryCondition::intentional_quit
                && session->simulation().outcome()->winner == 2, "a received real Quit still gives the opponent the win");
    } else for (auto* session : sessions) {
        expect(!session->simulation().outcome() && session->departure_outcomes().empty(), "neither isolated peer wins or manufactures a departure");
        const auto replay = session->simulation().record();
        expect(replay.final_tick_count == session->simulation().completed_tick(), "loss replay records only completed prefix");
        for (const auto& command : replay.commands)
            expect(!std::holds_alternative<tactical::QuitPayload>(command.payload), "loss replay contains no invented Quit");
        expect(take(tactical::parse_replay(take(tactical::write_replay(replay)))) == replay, "completed loss prefix roundtrips without a binary format change");
    }
    expect(host->link(1) == net::LinkState::open && client->link(0) == net::LinkState::open,
        "mutual loss is injected application silence, not a transport-close shortcut");
}

}
int main() {
    try {
#if defined(EAWR_TEST_GNS)
        if (!TestTransport::available()) { std::cout << "GNS disabled: lockstep test skipped\n"; return 77; }
#endif
        battle(); battle(DepartureCase::closed); battle(DepartureCase::silent);
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            mutual_loss_or_real_quit(workers, false); mutual_loss_or_real_quit(workers, true);
            mutual_loss_or_real_quit(workers, false, true);
        }
        std::cout << "Transport lockstep scripted battle passed\n"; return 0;
    }
    catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
