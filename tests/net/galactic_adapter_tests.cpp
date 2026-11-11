#include "../galactic/galactic_support.hpp"
#include "fake_transport.hpp"

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/synthetic.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/net/galactic_adapter.hpp"
#include "eawr/net/galactic_recovery.hpp"
#include "eawr/checkpoint/game.hpp"
#include "../../src/sim/galactic/checkpoint_fields_internal.hpp"
#include <thread>
#include <chrono>
#include "eawr/platform/sim_workers.hpp"
#include "../../src/script/foc/galactic_ai_battle_listener.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// #3769: the production galaxy adapter under the REAL LockstepSession (fake delayed transport, desync monitor):
// seat authentication, payload classification, install at the agreed key, stepping only on the common frontier,
// and equal per-tick digests against an unnetworked reference at 1/2/4/8 workers.
namespace {

using galactic_test::expect;
namespace net = eawr::net;
namespace galactic = eawr::sim::galactic;
namespace math = eawr::sim::math;
using eawr::campaign::GameSession;
using eawr::testing::FakeDelivery;
using eawr::testing::FakeNetwork;
using eawr::testing::FakeTransport;
using Bytes = std::vector<std::uint8_t>;

template <class T> T require(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
void require(const eawr::core::Result<void>& result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
}

galactic::GalacticSetup versus_setup() {
    eawr::campaign::SyntheticCampaign campaign;
    campaign.name = "net-galactic-versus";
    campaign.galaxy.seed = 31;
    campaign.galaxy.planets = 24;
    campaign.galaxy.players = 3;
    campaign.galaxy.fleets_per_player = 2;
    campaign.galaxy.members_per_fleet = 3;
    campaign.galaxy.debug_commands = true;
    campaign.fiscal_cycle_seconds = math::Fixed::from_decimal("2.5").value();
    return require(eawr::campaign::build_synthetic_campaign(campaign));
}
// Versus with the recorded seat-control policy (WGS-P01..P04): a leaving human is replaced by its AI fallback seat.
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
galactic::GalacticSetup coop_policy_setup(const std::uint32_t humans) {
    auto setup = policy_setup();
    setup.seats.clear();
    for (std::uint32_t id = 1; id <= humans; ++id)
        setup.seats.push_back({{id}, 1, eawr::sim::SeatKind::human, id - 1, true, 1});
    for (std::uint32_t player = 1; player <= 3; ++player)
        setup.seats.push_back({{humans + player}, player, eawr::sim::SeatKind::ai, 0, player != 1, 0});
    setup.seat_control->teams = {{1, {1}, false}};
    setup.seat_control->controllers[1].kind = galactic::FactionControl::ai;
    setup.seat_control->controllers[0].configuration = {7, 11};
    setup.seat_control->controllers[0].state = {13, 17};
    return setup;
}
// Four humans on one side (seats 1, 4, 5, 6) against two AI seats.
galactic::GalacticSetup coop_setup() {
    eawr::campaign::SyntheticCampaign campaign;
    campaign.name = "net-galactic-coop";
    campaign.galaxy.seed = 37;
    campaign.galaxy.planets = 24;
    campaign.galaxy.players = 3;
    campaign.galaxy.fleets_per_player = 2;
    campaign.galaxy.members_per_fleet = 3;
    campaign.galaxy.coop_seat = true;
    campaign.galaxy.debug_commands = true;
    auto setup = require(eawr::campaign::build_synthetic_campaign(campaign));
    for (std::uint32_t id = 5; id <= 6; ++id) setup.seats.push_back({{id}, 1, eawr::sim::SeatKind::human, id - 1, true, 1});
    return setup;
}

std::vector<eawr::sim::SeatId> human_seats(const galactic::GalacticSetup& setup) {
    std::vector<eawr::sim::SeatId> seats;
    for (const auto& seat : setup.seats) if (seat.kind == eawr::sim::SeatKind::human) seats.push_back(seat.seat_id);
    std::sort(seats.begin(), seats.end());
    return seats;
}
galactic::PlayerId player_of(const galactic::GalacticSetup& setup, const eawr::sim::SeatId id) {
    for (const auto& seat : setup.seats) if (seat.seat_id == id) return seat.player;
    return 0;
}

// What a human seat does at its completed tick `tick`: ordinary ownership and orbit orders (some of them
// unauthorized, refused identically on every peer at execution), and the first seat pauses and resumes.
std::optional<galactic::CommandPayload> intent(const galactic::GalacticSetup& setup, const eawr::sim::SeatId seat,
    const std::uint64_t tick, const bool leader) {
    const auto planets = setup.planets.size();
    if (leader && tick == 11) return galactic::PauseGalaxy{};
    if (leader && tick == 19) return galactic::ResumeGalaxy{};
    if (leader && tick == 23) return galactic::SetSpeed{false, math::Fixed::from_raw(2 * math::Fixed::scale)};
    if (tick % 3 == (seat.value % 3))
        return galactic::DebugSetPlanetOwner{setup.planets[(tick * 7 + seat.value) % planets].id, player_of(setup, seat)};
    if (tick % 4 == 1)
        return galactic::DebugSetFleetOrbit{setup.fleets[(tick + seat.value) % setup.fleets.size()].id,
            setup.planets[(tick * 11) % planets].id};
    return std::nullopt;
}

// Synthetic direct barrier: skip historical work while retaining ordinary production
// checkpoint decoding and all live lockstep/save paths. Only the two owner clocks and
// the galaxy session clock change; the simulation source has no test clock setter.
GameSession at_tick(const galactic::GalacticSetup& setup, std::uint64_t tick) {
    namespace archive = galactic::checkpoint_detail;
    auto fresh = require(GameSession::create(setup));
    if (tick == 0) return fresh;
    auto bytes = require(fresh.save_checkpoint_state());
    archive::Reader owner(bytes);
    std::uint32_t version{}; Bytes world; owner(version, world);
    const auto owner_tick_offset = bytes.size() - owner.remaining();
    archive::Reader galaxy(world);
    std::vector<galactic::Planet> planets; std::vector<galactic::Route> routes;
    std::vector<galactic::Fleet> fleets; galaxy(planets, routes, fleets);
    const auto world_tick_offset = world.size() - galaxy.remaining();
    const auto put_tick = [tick](Bytes& target, std::size_t offset) {
        for (unsigned byte = 0; byte < 8; ++byte)
            target[offset + byte] = static_cast<std::uint8_t>(tick >> (8U * byte));
    };
    put_tick(world, world_tick_offset);
    // completed tick:u64, next entity ID:u64, then GalacticClock::session_tick:u64.
    put_tick(world, world_tick_offset + 16);
    archive::Writer rewritten; rewritten(version, world);
    rewritten.bytes.insert(rewritten.bytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(owner_tick_offset), bytes.end());
    put_tick(rewritten.bytes, owner_tick_offset);
    auto result = require(GameSession::create(setup));
    require(result.load_checkpoint_state(rewritten.bytes));
    return result;
}

struct Rig {
    struct Options {
        std::uint64_t delay{3}, final_tick{60};
        net::GalacticClockPolicy clock_policy{net::GalacticClockPolicy::unrestricted};
        std::uint64_t completeness_timeout_ms{100'000};
        std::vector<std::size_t> workers{1};
        std::optional<eawr::testing::WanProfile> wan;
        bool monitor{true};
        bool departures{false};
        std::size_t observers{0}; // trailing peers are observers on AI seats
        std::uint64_t initial_tick{};
    };
    Rig(galactic::GalacticSetup initial, const Options rig_options)
        : setup(std::move(initial)), options(rig_options), humans(human_seats(setup)),
          network(static_cast<std::uint32_t>(humans.size() + rig_options.observers)) {
        const auto peers = static_cast<std::uint32_t>(humans.size() + options.observers);
        if (options.wan) network.emulate(*options.wan);
        std::vector<net::PeerSeat> seats;
        for (std::size_t peer = 0; peer < peers; ++peer) {
            if (peer < humans.size()) seats.push_back({humans[peer], true});
            else seats.push_back({eawr::sim::SeatId{static_cast<std::uint32_t>(3 + peer - humans.size())}, false});
        }
        for (net::PeerIndex peer = 0; peer < peers; ++peer) {
            executors.push_back(std::make_unique<eawr::platform::ThreadWorkerAdapter>(options.workers[peer % options.workers.size()]));
            games.push_back(std::make_unique<GameSession>(at_tick(setup, options.initial_tick)));
            if (peer != 0) games.back()->set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
            net::GalacticAdapterOptions adapter_options;
            adapter_options.clock_policy = options.clock_policy;
            adapters.push_back(std::make_unique<net::GalacticSessionAdapter>(*games.back(), *executors.back(), adapter_options));
            transports.push_back(std::make_unique<FakeTransport>(network, peer));
            net::DesyncMonitor* monitor = nullptr;
            if (options.monitor) {
                net::DesyncOptions monitor_options;
                monitor_options.initial_tick = options.initial_tick;
                monitor_options.input_delay = options.delay;
                monitor_options.exchange_every = 10;
                monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), monitor_options));
                monitor = monitors.back().get();
            }
            net::SessionOptions session_options;
            session_options.initial_tick = options.initial_tick;
            session_options.input_delay = options.delay;
            session_options.final_tick_count = options.final_tick;
            session_options.completeness_timeout_ms = options.completeness_timeout_ms;
            if (options.departures) session_options.unconfirmed_departure = net::UnconfirmedDeparture::continue_alone;
            sessions.push_back(require(net::LockstepSession::create(*adapters.back(), *transports.back(), seats, session_options, monitor)));
            if (options.departures)
                sessions.back().enable_departures([](eawr::sim::SeatCommandKey key) {
                    return net::encode_galactic_input(key, galactic::SeatLeave{});
                });
            sessions.back().set_stage_observer([this, peer](std::string_view stage, const net::SeatInput& input, std::uint64_t) {
                if (peer == 0 && stage == "seal") sealed_by_zero.push_back(input);
            });
        }
        hashes.resize(peers);
        inputs.resize(peers);
        submitted.assign(peers, std::numeric_limits<std::uint64_t>::max());
        departures.resize(peers);
    }

    // One fake millisecond per round, every peer submits its scripted intent once per completed tick.
    void run(const std::uint64_t max_rounds = 20'000, const std::function<void(std::uint64_t)>& each_round = {}) {
        for (std::uint64_t round = 0; round < max_rounds; ++round) {
            network.round = round;
            for (net::PeerIndex peer = 0; peer < sessions.size(); ++peer) {
                if (finished[peer]) continue;
                const auto tick = adapters[peer]->completed_tick();
                if (peer < humans.size() && tick < options.final_tick && submitted[peer] != tick) {
                    submitted[peer] = tick;
                    if (const auto action = intent(setup, humans[peer], tick, peer == 0)) {
                        const auto key = sessions[peer].submit([&](auto stamp) { return net::encode_galactic_input(stamp, *action); });
                        if (key) log.push_back({{key.value().tick, key.value().seat, key.value().sequence}, *action});
                    }
                }
                auto result = sessions[peer].pump(round);
                if (!result) { failure = result.error(); return; }
                for (const auto& key : result.value().departures) departures[peer].push_back(key);
                if (result.value().completed) {
                    hashes[peer].push_back(result.value().completed->state_sha256);
                    auto input = require(adapters[peer]->canonical_input());
                    inputs[peer].push_back(std::move(input));
                    static_cast<void>(adapters[peer]->take_tick());
                }
                if (adapters[peer]->completed_tick() == options.final_tick && (!options.monitor || monitors[peer]->agreed_end()))
                    finished[peer] = true;
            }
            if (each_round) each_round(round);
            if (std::all_of(finished.begin(), finished.begin() + static_cast<std::ptrdiff_t>(sessions.size()), [](const bool value) { return value; })) return;
        }
    }

    galactic::GalacticSetup setup;
    Options options;
    std::vector<eawr::sim::SeatId> humans;
    FakeNetwork network;
    std::vector<std::unique_ptr<eawr::platform::ThreadWorkerAdapter>> executors;
    std::vector<std::unique_ptr<GameSession>> games;
    std::vector<std::unique_ptr<net::GalacticSessionAdapter>> adapters;
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    std::vector<net::LockstepSession> sessions;
    std::vector<std::vector<std::string>> hashes;
    std::vector<std::vector<Bytes>> inputs;
    std::vector<std::uint64_t> submitted;
    std::vector<std::vector<eawr::sim::SeatCommandKey>> departures;
    std::vector<galactic::GameCommand> log;
    std::vector<net::SeatInput> sealed_by_zero;
    std::array<bool, 16> finished{};
    std::optional<eawr::core::Diagnostic> failure;
};

// The unnetworked oracle: the same stamped commands in one GameSession.
std::vector<std::string> reference(const galactic::GalacticSetup& setup, std::vector<galactic::GameCommand> commands,
    const std::uint64_t ticks, const std::size_t workers) {
    std::sort(commands.begin(), commands.end(), [](const auto& left, const auto& right) { return left.key < right.key; });
    auto game = require(GameSession::create(setup));
    for (const auto& command : commands) require(game.submit(command));
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    std::vector<std::string> hashes;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = require(game.step(executor));
        hashes.push_back(stepped.state_sha256.empty() ? stepped.state_hash.get() : stepped.state_sha256);
    }
    return hashes;
}

void classification_and_codec() {
    const auto setup = versus_setup();
    GameSession game = require(GameSession::create(setup));
    const eawr::sim::InlineExecutor executor;
    net::GalacticAdapterOptions galaxy_only;
    galaxy_only.child_battle_commands = false;
    const net::GalacticSessionAdapter adapter(game, executor, galaxy_only);
    const auto stamp = [](std::uint32_t seat, std::uint64_t tick = 5) { return eawr::sim::SeatCommandKey{tick, {seat}, 1}; };
    const auto planet = setup.planets.front().id;
    const auto accepted = [&](std::uint32_t seat, const galactic::CommandPayload& payload) {
        return adapter.validate({stamp(seat), require(net::encode_galactic_input(stamp(seat), payload))});
    };

    expect(static_cast<bool>(accepted(1, galactic::PauseGalaxy{})), "a human seat may pause");
    expect(static_cast<bool>(accepted(1, galactic::DeployGroundAbility{100, planet}))
        && net::galactic_payload_authority(galactic::DeployGroundAbility{}) == net::GalacticPayloadAuthority::human_wire,
        "generic ground deployment is typed human wire; live owner/location admission belongs to the host");
    expect(static_cast<bool>(accepted(2, galactic::DebugSetPlanetOwner{planet, 2})), "versus seat 2 is a human seat");
    expect(net::galactic_payload_authority(galactic::PostLoadFixup{}) == net::GalacticPayloadAuthority::engine_only
            && net::galactic_payload_authority(galactic::SeatRejoin{}) == net::GalacticPayloadAuthority::engine_only
            && net::galactic_payload_authority(galactic::ObserverCommand{}) == net::GalacticPayloadAuthority::engine_only
            && net::galactic_payload_authority(galactic::RecheckOrbit{}) == net::GalacticPayloadAuthority::engine_only
            && net::galactic_payload_authority(galactic::SeatLeave{}) == net::GalacticPayloadAuthority::human_wire,
        "payload classification");
    for (const auto& payload : std::vector<galactic::CommandPayload>{
             galactic::PostLoadFixup{}, galactic::SeatRejoin{}, galactic::RecheckOrbit{planet}}) {
        const auto refused = accepted(1, payload);
        expect(!refused && refused.error().code == net::session_errors::admission, "engine-only payloads never cross the human wire");
    }
    const auto rejoin = accepted(1, galactic::SeatRejoin{});
    expect(!rejoin && rejoin.error().message.find("rejoin is disabled") != std::string::npos,
        "Galactic rejoin refusal explains the M-GC1 policy");
    // Engine seats (AI seat 3) and unbound seats are not network seats.
    const auto ai = accepted(3, galactic::PauseGalaxy{});
    expect(!ai && ai.error().code == net::session_errors::admission, "an AI seat cannot send human wire input");
    const auto unbound = accepted(77, galactic::PauseGalaxy{});
    expect(!unbound && unbound.error().code == net::session_errors::admission, "an unbound seat is refused");
    // A caller can explicitly disable child command admission.
    const auto tactical = accepted(1, galactic::TacticalLayerCommand{1, 5, {}});
    expect(!tactical && tactical.error().code == net::session_errors::admission, "a galaxy-only adapter refuses tactical-layer commands");
    // Debug commands follow the agreed setup flag.
    auto plain = setup;
    plain.debug_commands = false;
    GameSession plain_game = require(GameSession::create(plain));
    const net::GalacticSessionAdapter plain_adapter(plain_game, executor);
    const auto debug = plain_adapter.validate({stamp(1), require(net::encode_galactic_input(stamp(1), galactic::DebugSetPlanetOwner{planet, 1}))});
    expect(!debug && debug.error().code == net::session_errors::admission, "debug commands need the agreed setup flag");

    // Strict codec: noncanonical bytes are refused.
    auto bytes = require(net::encode_galactic_input(stamp(1), galactic::DebugSetPlanetOwner{planet, 1}));
    const auto decoded = require(net::decode_galactic_input({stamp(1), bytes}));
    expect(decoded.key.tick == 5 && decoded.payload == galactic::CommandPayload{galactic::DebugSetPlanetOwner{planet, 1}}, "codec round trip");
    auto longer = bytes;
    longer.push_back(0);
    expect(!net::decode_galactic_input({stamp(1), longer}), "trailing bytes are refused");
    auto bad_layer = bytes;
    bad_layer[0] = 9;
    expect(!net::decode_galactic_input({stamp(1), bad_layer}), "unknown layer is refused");
    auto bad_opcode = bytes;
    bad_opcode[1] = 0xff;
    expect(!net::decode_galactic_input({stamp(1), bad_opcode}), "unknown opcode is refused");
    expect(!net::decode_galactic_input({stamp(1), {}}), "empty input is refused");
    expect(!net::decode_galactic_input({stamp(0), bytes}), "seat zero is refused");

    expect(adapter.is_departure({stamp(1), require(net::encode_galactic_input(stamp(1), galactic::SeatLeave{}))}), "seat leave is the departure input");
    expect(!adapter.is_departure({stamp(1), bytes}), "ordinary input is not a departure");

    // install only takes the executing tick's complete, ordered inputs.
    GameSession target = require(GameSession::create(setup));
    net::GalacticSessionAdapter installer(target, executor);
    const net::SeatInput late{stamp(1, 3), require(net::encode_galactic_input(stamp(1, 3), galactic::PauseGalaxy{}))};
    expect(!installer.install(std::array{late}), "install refuses input stamped for another tick (unsealed or late)");
    const net::SeatInput now{stamp(1, 0), require(net::encode_galactic_input(stamp(1, 0), galactic::PauseGalaxy{}))};
    const net::SeatInput before{{0, {1}, 0}, now.bytes};
    expect(!installer.install(std::array{now, before}), "install refuses non-canonical order");
    const net::SeatInput unsupported_leave{stamp(1, 0),
        require(net::encode_galactic_input(stamp(1, 0), galactic::SeatLeave{}))};
    const auto no_policy = installer.install(std::array{unsupported_leave});
    expect(!no_policy && no_policy.error().message.find("seat-control fallback") != std::string::npos,
        "a missing dropout policy cannot silently remove a network voter without changing the galaxy");
    expect(static_cast<bool>(installer.install(std::array{now})), "install takes the executing tick");
    const auto stepped = require(installer.step());
    expect(stepped.completed_tick == 1 && installer.completed_tick() == 1, "one step is one tick");
    expect(target.galaxy().clock().pause_depth == 1, "the installed pause executed at its agreed tick");
    const auto input = require(installer.canonical_input());
    expect(!input.empty(), "canonical input records the executed command");
}

void child_hash_and_execution_refusal() {
    const auto setup = versus_setup();
    auto game = require(GameSession::create(setup));
    const eawr::sim::InlineExecutor executor;
    net::GalacticAdapterOptions options;
    options.child_battle_commands = true;
    net::GalacticSessionAdapter adapter(game, executor, options);
    eawr::campaign::ConflictRecord entry;
    entry.id = game.galaxy().next_entity_id(); entry.planet = setup.planets.front().id;
    entry.invader = 1; entry.defender = 2;
    entry.tactical.content_identity = setup.content_identity;
    entry.tactical.first_free_entity_id = entry.id + 1;
    entry.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    entry.seats = {{{1}, 1}, {{2}, 2}};
    require(game.stage_battle(entry, {}, eawr::campaign::ScriptRequirement::none));
    require(adapter.install({}));
    const auto begin = require(adapter.step());
    expect(begin.completed_tick == 1 && game.battle() && game.battle()->completed_tick() == 0,
        "the sealed galaxy tick is the child's Begin barrier");
    expect(begin.state_sha256 == game.state_sha256() && begin.state_sha256 != game.galaxy().state_sha256(),
        "the adapter digest is the whole game, including child context at Begin");
    // #5519: the exchange encoding with a child battle open is the serial galaxy encoding followed by the child's, for
    // every worker count (parallel sections and parallel child records are only a faster way to the same bytes).
    {
        const auto serial_galaxy = game.galaxy().canonical_state_bytes();
        const auto serial_child = game.battle()->canonical_state_bytes();
        std::optional<Bytes> first;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter parallel(workers);
            expect(game.battle()->canonical_state_bytes(parallel) == serial_child, "the child's parallel encoding equals its serial encoding");
            net::GalacticSessionAdapter exchange(game, parallel, options);
            const auto state = exchange.canonical_state();
            expect(state.size() > serial_galaxy.size() && std::equal(serial_galaxy.begin(), serial_galaxy.end(), state.begin()),
                "the exchange encoding starts with the serial galaxy encoding");
            expect(std::search(state.begin(), state.end(), serial_child.begin(), serial_child.end()) != state.end(),
                "the exchange encoding carries the child's serial encoding");
            if (!first) first = state;
            else expect(state == *first, "the exchange encoding with a child is equal at every worker count");
        }
    }
    const eawr::sim::SeatCommandKey key{1, {1}, 1};
    const net::SeatInput stale{key, require(net::encode_galactic_input(key,
        galactic::TacticalLayerCommand{entry.id + 10, 5, {}}))};
    require(adapter.validate(stale)); // active conflict is never a wire/admission fact
    require(adapter.install(std::array{stale}));
    const auto battle = require(adapter.step());
    expect(adapter.refused().size() == 1 && battle.completed_tick == 2,
        "another conflict's tactical command is refused at execution without ending lockstep");
    const auto tick = adapter.take_tick();
    expect(tick && tick->diagnostics.size() == 1 && tick->diagnostics.front().message == adapter.refused().front().message,
        "execution refusal is published with its completed tick");
    const auto input = require(adapter.canonical_input());
    expect(input.size() > 4 && input.front() == 1, "canonical input retains the refused tactical envelope");
    expect(battle.state_sha256 == game.state_sha256() && battle.state_sha256 != game.galaxy().state_sha256(),
        "battle ticks keep the whole-game digest");
}

void unavailable_battle_callback_coop() {
    namespace ga = eawr::script::foc::galactic_ai;
    namespace auth = eawr::script::authoritative;
    namespace ai = eawr::script::foc::ai;
    namespace cp = eawr::campaign;
    std::vector<std::string> reference_hashes;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        Rig::Options options; options.workers = {workers, 2}; options.final_tick = 4; options.monitor = false;
        auto setup = coop_policy_setup(2);
        setup.rules.maximum_fleet_movement_distance = math::Fixed{}; // Opt in to the native graph before movement admission.
        Rig rig(std::move(setup), options);
        for (auto& game : rig.games) {
            const auto fleet = std::find_if(rig.setup.fleets.begin(), rig.setup.fleets.end(), [](const auto& row) { return row.owner == 2; });
            const auto enemy = std::find_if(rig.setup.fleets.begin(), rig.setup.fleets.end(), [](const auto& row) { return row.owner == 1; });
            cp::ConflictRecord conflict; conflict.id = game->galaxy().next_entity_id(); conflict.planet = fleet->orbit;
            conflict.invader = 2; conflict.defender = 1; conflict.tactical.content_identity = rig.setup.content_identity;
            conflict.tactical.first_free_entity_id = conflict.id + 1;
            conflict.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
            for (const auto* row : {&*fleet, &*enemy})
                conflict.forces.push_back({row->members.front().id, row->members.front().type, row->owner, row->id});
            std::sort(conflict.forces.begin(), conflict.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
            conflict.seats = {{{1}, 1}, {{2}, 1}};
            require(game->configure_galactic_ai_services([&](galactic::GalacticSession& world) {
                expect(world.graph() && world.graph()->node(fleet->orbit),
                    "co-op battle fixture has a native movement destination before registering its listener");
                auto actions = std::make_shared<ga::NativeActions>(world, std::make_shared<ai::GalacticFunding>(),
                    ga::NativeActions::Placement{}, std::map<galactic::PlayerId, math::Fixed>{});
                actions->bind_admission_gate([] { return false; });
                actions->bind_movement([](auto, const auto&) { return eawr::core::Result<ga::NativeActions::MovementInput>::success({}); },
                    [] { return true; }, [](auto, auto) { return eawr::core::Result<void>::success(); });
                ga::ActionRequest move; move.kind = ga::ActionKind::move; move.player = 2; move.object = fleet->id;
                move.target = fleet->orbit; move.members = {fleet->members.front().id}; move.taskforce = move.plan = move.block = 1;
                expect(require(actions->admit(move)).admitted, "AI TaskForce has a native movement listener before the co-op battle");
                cp::GalacticAiServices services; services.state = actions;
                // The production listener reads the notice's owned facts, as the runtime hands them over (A2.1).
                services.battle = [listener = ga::retained_movement_battle_listener(actions, {},
                    std::function<auth::ScriptScheduler*()>{[] { return nullptr; }})]
                    (const eawr::campaign::GalacticAiBattleNotice& notice) { return listener(eawr::campaign::galactic_ai_battle_facts(notice)); };
                return eawr::core::Result<cp::GalacticAiServices>::success(std::move(services));
            }));
            // Retained forces need no deployed tactical object for this boundary test.
            for (auto& force : conflict.forces) force.role = cp::ForceRole::retained;
            require(game->stage_battle(conflict, {}, cp::ScriptRequirement::none));
        }
        for (std::uint64_t round = 0; round < 1000; ++round) {
            rig.network.round = round;
            for (std::size_t peer = 0; peer < rig.sessions.size(); ++peer) {
                const auto tick = require(rig.sessions[peer].pump(round));
                if (tick.completed) rig.hashes[peer].push_back(tick.completed->state_sha256);
            }
            if (rig.adapters[0]->completed_tick() == options.final_tick && rig.adapters[1]->completed_tick() == options.final_tick) break;
        }
        expect(rig.hashes[0].size() == options.final_tick && rig.hashes[0] == rig.hashes[1],
            "two real co-op lockstep peers pass the former NET-0109 boundary with equal hashes");
        for (const auto& game : rig.games) {
            const auto notes = game->galaxy().load_notes();
            expect(game->mode() == cp::SessionMode::battle && game->battle()->completed_tick() == options.final_tick - 1
                && notes.size() == 1 && notes.front().code == "EAWR-LOAD-1236",
                "AI-initiated co-op battle continues with a LoadNote and no campaign-ending network error");
        }
        if (workers == 1) reference_hashes = rig.hashes[0];
        else expect(rig.hashes[0] == reference_hashes, "co-op degraded battle hashes agree on 1/2/4/8 workers");
    }
}

void loopback(const std::string& name, const galactic::GalacticSetup& setup, const std::optional<eawr::testing::WanProfile> wan) {
    std::optional<std::vector<std::string>> first_hashes;
    std::optional<Bytes> first_state;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        Rig::Options options;
        options.workers = {workers, 4U, 1U, 2U}; // peers deliberately mix worker counts
        options.wan = wan;
        Rig rig(setup, options);
        rig.run(60'000);
        const auto where = name + " workers=" + std::to_string(workers) + ": ";
        expect(!rig.failure, where + (rig.failure ? rig.failure->code + " " + rig.failure->message : ""));
        if (rig.failure) return;
        const auto oracle = reference(setup, rig.log, options.final_tick, workers);
        for (std::size_t peer = 0; peer < rig.sessions.size(); ++peer) {
            expect(rig.adapters[peer]->completed_tick() == options.final_tick, where + "every peer completes the agreed ticks and no more");
            expect(rig.hashes[peer] == rig.hashes[0], where + "equal per-tick digests on every peer");
            expect(rig.inputs[peer] == rig.inputs[0], where + "equal canonical input on every peer");
            expect(rig.monitors[peer]->agreed_end(), where + "the desync monitor agrees through the final tick");
            expect(rig.adapters[peer]->canonical_state() == rig.adapters[0]->canonical_state(), where + "equal final canonical state");
        }
        expect(rig.hashes[0] == oracle, where + "networked digests equal the unnetworked reference");
        expect(!rig.log.empty(), where + "commands were exchanged");
        if (workers == 1) { first_hashes = rig.hashes[0]; first_state = rig.adapters[0]->canonical_state(); }
        else {
            expect(rig.hashes[0] == *first_hashes, where + "digests equal the 1-worker run");
            expect(rig.adapters[0]->canonical_state() == *first_state, where + "canonical state equals the 1-worker run");
        }
        // Pause is session time: the clock froze while the tick counter went on to its end.
        if (workers == 1) {
            const auto& clock = rig.games[0]->galaxy().clock();
            expect(clock.pause_depth == 0, where + "the scripted resume released the pause");
        }
    }
}

void shared_clock_limit_at_execution() {
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        Rig::Options options;
        options.final_tick = 40;
        options.workers = {1, workers};
        options.clock_policy = net::GalacticClockPolicy::limited;
        Rig rig(versus_setup(), options);
        const auto send = [&](net::PeerIndex peer, const galactic::CommandPayload& payload) {
            return require(rig.sessions[peer].submit([&](auto key) { return net::encode_galactic_input(key, payload); }));
        };
        const auto pause = send(0, galactic::PauseGalaxy{});
        // #4574/#5355: the per-seat speed budget (2 per 10 session seconds); the pause above does not spend any of it.
        // The shared 2x (the multiplayer maximum) applies; a repeat of the same value still spends a slot.
        const auto shared = math::Fixed::from_raw(galactic::multiplayer_speed_max_raw);
        auto speed = send(0, galactic::SetSpeed{false, shared});
        for (std::size_t extra = 1; extra < net::ClockThrottle::speed_changes_per_window; ++extra)
            speed = send(0, galactic::SetSpeed{false, shared});
        const auto refused_speed = send(0, galactic::SetSpeed{false, math::Fixed::from_raw(math::Fixed::scale / 2)});
        const auto other_resume = send(1, galactic::ResumeGalaxy{});
        // The other human's budget admits 2.5x; the sim refuses it (above 2x with two humans) on both peers.
        const auto too_fast = send(1, galactic::SetSpeed{false, math::Fixed::from_raw(5 * math::Fixed::scale / 2)});
        expect(pause.tick == speed.tick && speed.tick == refused_speed.tick && other_resume.tick == pause.tick
            && too_fast.tick == pause.tick, "pause/speed requests are stamped at the same agreed frontier");
        std::array<bool, 2> observed{};
        for (std::uint64_t round = 0; round < 20'000; ++round) {
            rig.network.round = round;
            for (net::PeerIndex peer = 0; peer < 2; ++peer) {
                if (rig.monitors[peer]->agreed_end()) continue;
                const auto pumped = require(rig.sessions[peer].pump(round));
                if (!pumped.completed) continue;
                const auto& completed = *pumped.completed;
                rig.hashes[peer].push_back(completed.state_sha256);
                rig.inputs[peer].push_back(require(rig.adapters[peer]->canonical_input()));
                const auto published = rig.adapters[peer]->take_tick();
                expect(published && published->state_sha256 == completed.state_sha256
                    && published->state_hash.get() == completed.state_sha256
                    && rig.adapters[peer]->state_hash() == completed.state_sha256,
                    "published, current and completed hashes all include the clock history");
                if (completed.completed_tick == pause.tick + 1) {
                    observed[peer] = true;
                    expect(rig.games[peer]->galaxy().clock().pause_depth == 0,
                        "another human may resume at the agreed tick with its own budget");
                    expect(rig.games[peer]->galaxy().clock().strategic_speed.raw() == galactic::multiplayer_speed_max_raw,
                        "the shared 2x applies on the same completed tick on both peers");
                    const auto& events = published->galaxy->events();
                    expect(std::count_if(events.begin(), events.end(), [&](const galactic::GalacticEvent& event) {
                        return event.kind == galactic::GalacticEventKind::command_rejected && event.producer == rig.humans[1]
                            && event.reason == galactic::GalacticRejectReason::not_authorized; }) == 1,
                        "the other human's 2.5x is refused by the sim at the same completed tick on both peers");
                    expect(rig.adapters[peer]->refused().size() == 1,
                        "the speed change past the budget is refused during execution, not while receiving");
                    const eawr::sim::SeatCommandKey probe{20, rig.humans[0], 99};
                    expect(bool(rig.adapters[peer]->validate({probe, require(net::encode_galactic_input(probe, galactic::PauseGalaxy{}))})),
                        "wire admission does not depend on the already exhausted execution budget");
                }
            }
            if (rig.monitors[0]->agreed_end() && rig.monitors[1]->agreed_end()) break;
        }
        expect(observed[0] && observed[1] && rig.hashes[0].size() == options.final_tick,
            "both peers execute the complete clock fixture");
        expect(rig.hashes[0] == rig.hashes[1] && rig.inputs[0] == rig.inputs[1],
            "clock acceptance/refusal produces equal digests and canonical input on 1/2/4/8 workers");
        expect(rig.adapters[0]->refused().size() == 1 && rig.adapters[1]->refused().size() == 1
            && rig.adapters[0]->refused()[0].message == rig.adapters[1]->refused()[0].message
            && rig.adapters[0]->refused()[0].message.find("speed changes are limited to 2 per 10 session seconds") != std::string::npos
            && rig.adapters[0]->refused()[0].message.find("next is allowed in 10 s") != std::string::npos,
            "the speed change past the budget is refused identically on both peers, naming the limit and the wait");
        const net::GalacticSessionAdapter unrestricted(*rig.games[0], *rig.executors[0]);
        expect(unrestricted.state_hash() != rig.adapters[0]->state_hash(),
            "the named clock policy/history participate in the agreed state hash");
    }
}

void pause_progress_and_no_local_ticks() {
    const auto setup = versus_setup();
    Rig::Options options;
    options.final_tick = 40;
    Rig rig(setup, options);
    std::uint32_t paused_ticks = 0;
    rig.run(20'000, [&](std::uint64_t) {
        const auto& game = *rig.games[0];
        if (game.galaxy().clock().pause_depth > 0) ++paused_ticks;
    });
    expect(!rig.failure && paused_ticks > 0, "the pause window was observed");
    expect(rig.adapters[0]->completed_tick() == 40 && rig.adapters[1]->completed_tick() == 40,
        "session ticks, not galaxy time, are the shared steps: a pause adds none and removes none");
    expect(rig.hashes[0] == rig.hashes[1], "paused input progress keeps digests equal");

    // A lone peer takes no extra tick: without the other batch it cannot leave tick 0.
    auto lone_options = options;
    lone_options.monitor = false; // the batch wait, not the monitor's setup barrier, is what is observed here
    Rig lone(setup, lone_options);
    for (std::uint64_t round = 0; round < 500; ++round) {
        lone.network.round = round;
        const auto result = lone.sessions[0].pump(round);
        expect(static_cast<bool>(result), "waiting is not an error");
    }
    expect(lone.adapters[0]->completed_tick() == 0, "no local extra ticks while a sealed batch is missing");
    const auto& waiting = lone.sessions[0].waiting_for();
    expect(waiting && waiting->tick == 0 && waiting->peers == std::vector<net::PeerIndex>{1}, "the missing peer is named");
}

void held_and_duplicate_transport() {
    const auto setup = versus_setup();
    Rig::Options options;
    options.final_tick = 50;
    Rig rig(setup, options);
    // Peer 1's stream is held back for a while, delayed unevenly, and every batch is duplicated.
    rig.network.fault = [](net::PeerIndex from, net::PeerIndex, std::span<const std::uint8_t> frame) {
        FakeDelivery action;
        if (frame[0] == net::sealed_batch_kind) {
            action.duplicate = true;
            action.duplicate_delay = 3;
            if (from == 1) action.ordered_delay = 11;
        }
        return action;
    };
    rig.run(60'000);
    expect(!rig.failure, "held and duplicated stream");
    for (std::size_t peer = 0; peer < 2; ++peer) {
        expect(rig.adapters[peer]->completed_tick() == 50, "delay and duplicates add no shared step");
        expect(rig.hashes[peer] == rig.hashes[0], "delayed and duplicated batches keep digests equal");
    }
    expect(rig.hashes[0] == reference(setup, rig.log, 50, 1), "delayed run equals the reference");
}

void racing_inputs_are_canonical() {
    const auto setup = versus_setup();
    Rig::Options options;
    options.final_tick = 24;
    options.monitor = false;
    Rig rig(setup, options);
    const auto planet = setup.planets.front().id;
    // Both seats take the same planet at the same stamped tick; canonical order (tick, seat) decides, not arrival.
    std::array<bool, 2> sent{};
    rig.network.fault = [](net::PeerIndex from, net::PeerIndex, std::span<const std::uint8_t> frame) {
        FakeDelivery action;
        if (frame[0] == net::sealed_batch_kind && from == 0) action.ordered_delay = 5;
        return action;
    };
    for (std::uint64_t round = 0; round < 20'000; ++round) {
        rig.network.round = round;
        for (net::PeerIndex peer = 0; peer < 2; ++peer) {
            if (!sent[peer] && rig.adapters[peer]->completed_tick() == 6) {
                sent[peer] = true;
                require(rig.sessions[peer].submit([&](auto stamp) {
                    return net::encode_galactic_input(stamp, galactic::DebugSetPlanetOwner{planet, peer == 0 ? 1U : 2U});
                }));
            }
            if (rig.adapters[peer]->completed_tick() < 24) require(rig.sessions[peer].pump(round));
        }
        if (rig.adapters[0]->completed_tick() == 24 && rig.adapters[1]->completed_tick() == 24) break;
    }
    for (std::size_t peer = 0; peer < 2; ++peer) {
        const auto planets = rig.games[peer]->galaxy().snapshot()->planets();
        const auto found = std::find_if(planets.begin(), planets.end(), [&](const auto& value) { return value.id == planet; });
        expect(found != planets.end() && found->owner == 2, "the later seat in canonical order wins the same-tick race on every peer");
        expect(rig.games[peer]->state_sha256() == rig.games[0]->state_sha256(), "racing inputs end in equal state");
    }
}

void refusals_through_the_session() {
    const auto setup = versus_setup();
    const auto planet = setup.planets.front().id;
    const auto forge = [&](const std::uint32_t seat, const galactic::CommandPayload& payload, bool observer = false) {
        Rig::Options options;
        options.monitor = false;
        options.observers = observer ? 1 : 0;
        Rig rig(setup, options);
        // Peer 1 (or the observer) is a bare stream here; the frame is a validly framed sealed batch.
        const net::PeerIndex sender = observer ? 2 : 1;
        const eawr::sim::SeatCommandKey key{0, {seat}, 1};
        net::SealedBatch batch{sender, 0, 0, {{key, require(net::encode_galactic_input(key, payload))}}};
        const auto frame = require(net::encode_batch(batch));
        rig.network.deliver(sender, 0, frame);
        rig.network.round = 5;
        return rig.sessions[0].pump(5);
    };
    const auto wrong = forge(1, galactic::PauseGalaxy{});
    expect(!wrong && wrong.error().code == net::session_errors::admission, "a batch cannot issue for another peer's seat");
    const auto engine = forge(2, galactic::PostLoadFixup{});
    expect(!engine && engine.error().code == net::session_errors::admission, "an engine-only payload in a sealed batch fails the session");
    const auto observer_obs = forge(3, galactic::PauseGalaxy{}, true);
    expect(!observer_obs && observer_obs.error().code == net::session_errors::admission, "an observer seat cannot seal commands");
    const auto ai = forge(3, galactic::DebugSetPlanetOwner{planet, 3});
    expect(!ai && ai.error().code == net::session_errors::admission, "the sender binding refuses a stamp for a seat the peer does not own");

    // The observer peer cannot submit locally either.
    Rig::Options options;
    options.monitor = false;
    options.observers = 1;
    Rig rig(setup, options);
    const auto submitted = rig.sessions[2].submit([&](auto stamp) { return net::encode_galactic_input(stamp, galactic::PauseGalaxy{}); });
    expect(!submitted && submitted.error().code == net::session_errors::admission, "observers participate in completeness without input");

    // A late contradicting duplicate of an accepted batch is refused by the core.
    Rig duplicate_rig(setup, [] { Rig::Options o; o.monitor = false; return o; }());
    const eawr::sim::SeatCommandKey key{0, {2}, 1};
    const net::SealedBatch first{1, 0, 0, {{key, require(net::encode_galactic_input(key, galactic::PauseGalaxy{}))}}};
    const net::SealedBatch changed{1, 0, 0, {{key, require(net::encode_galactic_input(key, galactic::ResumeGalaxy{}))}}};
    duplicate_rig.network.deliver(1, 0, require(net::encode_batch(first)));
    duplicate_rig.network.deliver(1, 0, require(net::encode_batch(changed)));
    duplicate_rig.network.round = 9;
    const auto contradicted = duplicate_rig.sessions[0].pump(9);
    expect(!contradicted && contradicted.error().code == net::session_errors::conflicting_duplicate,
        "a contradicting replay of an accepted batch is refused");
}

void engine_input_runs_on_every_peer() {
    const auto setup = versus_setup();
    Rig::Options options;
    options.final_tick = 36;
    Rig rig(setup, options);
    std::size_t calls = 0;
    for (std::size_t peer = 0; peer < rig.adapters.size(); ++peer)
        rig.adapters[peer]->set_engine_source([&](const GameSession& game, std::uint64_t tick) {
            ++calls;
            std::vector<galactic::GameCommand> produced;
            // The AI seat acts deterministically from the executing tick and the world it can read.
            if (tick % 6 == 4)
                produced.push_back({{tick, {3}, tick + 1}, galactic::DebugSetPlanetOwner{game.galaxy().setup().planets[tick % 24].id, 3}});
            return eawr::core::Result<std::vector<galactic::GameCommand>>::success(std::move(produced));
        });
    rig.run(60'000);
    expect(!rig.failure && calls == 2 * 36, "the engine source runs once per executed tick on each peer");
    expect(rig.hashes[0] == rig.hashes[1] && rig.inputs[0] == rig.inputs[1], "engine input yields equal digests and canonical input");
    expect(std::any_of(rig.inputs[0].begin(), rig.inputs[0].end(), [](const Bytes& value) { return !value.empty(); }), "canonical input carries engine-side commands");
    // Engine commands never travel: peers sealed only their own seats' input.
    expect(std::none_of(rig.sealed_by_zero.begin(), rig.sealed_by_zero.end(), [](const net::SeatInput& input) { return input.key.seat.value == 3; }),
        "engine-seat input is never sealed on the wire");

    // An engine source cannot speak for a human seat.
    Rig bad(setup, options);
    bad.adapters[0]->set_engine_source([](const GameSession&, std::uint64_t tick) {
        std::vector<galactic::GameCommand> produced{{{tick, {1}, tick + 1}, galactic::PauseGalaxy{}}};
        return eawr::core::Result<std::vector<galactic::GameCommand>>::success(std::move(produced));
    });
    bad.run(5'000);
    expect(bad.failure && bad.failure->code == net::session_errors::admission, "an engine source for a human seat fails the step");
}

void departure_through_the_session() {
    const auto setup = policy_setup();
    Rig::Options options;
    options.final_tick = 40;
    options.monitor = false;
    options.departures = true;
    Rig rig(setup, options);
    bool left = false;
    for (std::uint64_t round = 0; round < 20'000 && !rig.failure; ++round) {
        rig.network.round = round;
        for (net::PeerIndex peer = 0; peer < 2; ++peer) {
            if (rig.adapters[peer]->completed_tick() >= 40) continue;
            if (peer == 1 && !left && rig.adapters[1]->completed_tick() == 10) {
                left = true;
                require(rig.sessions[1].submit([](auto stamp) { return net::encode_galactic_input(stamp, galactic::SeatLeave{}); }));
            }
            auto result = rig.sessions[peer].pump(round);
            if (!result) { rig.failure = result.error(); break; }
            for (const auto& key : result.value().departures) rig.departures[peer].push_back(key);
        }
        if (rig.adapters[0]->completed_tick() == 40) break;
    }
    expect(!rig.failure, "departure run");
    expect(rig.departures[0].size() == 1 && rig.departures[0][0].seat.value == 2 && rig.departures[0][0].tick == 13,
        "the leave executes at the agreed tick");
    const auto seats = rig.games[0]->galaxy().seats();
    const auto found = std::find_if(seats.begin(), seats.end(), [](const auto& seat) { return seat.seat_id.value == 2; });
    expect(found != seats.end() && !found->active, "the departed seat is inactive in the galaxy");
    // The survivor's session applied the recorded policy: the leaver's faction is now controlled by its AI fallback.
    const auto& control = rig.games[0]->galaxy().seat_control();
    bool ai_took_over = false;
    if (control)
        for (const auto& controller : control->control.controllers)
            if (controller.player == 2) ai_took_over = controller.kind == galactic::FactionControl::ai;
    expect(ai_took_over, "the AI fallback takes over the departed seat's faction");
    expect(rig.adapters[0]->completed_tick() == 40, "the survivor continues alone");
}

// GCMP-09 / WGS-P02/P04: staggered delivery and mixed worker counts must not turn local loss detection into
// a local SeatLeave. Compare every surviving peer through the agreed cutoff, then leave the rest of the coop team.
void agreed_departure_policy(const std::uint32_t humans, const bool disconnect) {
    const auto setup = humans == 2 ? policy_setup() : coop_policy_setup(humans);
    Rig::Options options;
    options.final_tick = 64;
    options.departures = true;
    options.workers = {1, 2, 4, 8};
    Rig rig(setup, options);
    rig.network.fault = [](net::PeerIndex from, net::PeerIndex to, std::span<const std::uint8_t>) {
        FakeDelivery action;
        action.ordered_delay = (from * 3 + to * 5) % 7;
        return action;
    };
    const net::PeerIndex leaver = humans == 2 ? 1U : 0U; // versus opponent / coop lead
    const net::PeerIndex survivor = humans == 2 ? 0U : humans - 1;
    std::vector<bool> inactive(humans);
    std::optional<eawr::sim::SeatCommandKey> submitted_leave;
    std::optional<net::DepartureEvent> agreed;
    std::vector<std::vector<net::DepartureEvent>> events(humans);
    bool triggered = false, rest_leaving = false, checked_lead = false;
    std::optional<eawr::sim::SeatCommandKey> last_leave;
    for (std::uint64_t round = 0; round < 50'000; ++round) {
        rig.network.round = round;
        if (!triggered && rig.adapters[leaver]->completed_tick() >= 12) {
            triggered = true;
            if (disconnect) {
                rig.network.open[leaver] = false;
                inactive[leaver] = true;
            } else {
                submitted_leave = require(rig.sessions[leaver].submit([](auto key) {
                    return net::encode_galactic_input(key, galactic::SeatLeave{});
                }));
            }
        }
        for (net::PeerIndex peer = 0; peer < humans; ++peer) {
            if (inactive[peer] || (rig.adapters[peer]->completed_tick() == options.final_tick
                && rig.monitors[peer]->agreed_end())) continue;
            const auto before = rig.adapters[peer]->completed_tick();
            const auto result = require(rig.sessions[peer].pump(round));
            if (result.completed) {
                rig.hashes[peer].push_back(result.completed->state_sha256);
                rig.inputs[peer].push_back(require(rig.adapters[peer]->canonical_input()));
                static_cast<void>(rig.adapters[peer]->take_tick());
            }
            for (const auto& event : result.departure_events) {
                expect(result.completed && result.completed->completed_tick == event.key.tick + 1
                    && before == event.key.tick, "departure is reported only after its agreed simulation tick");
                events[peer].push_back(event);
                if (event.key.seat == rig.humans[leaver]) {
                    if (!agreed) agreed = event;
                    expect(event == *agreed, "survivors agree the exact leave stamp and cause despite unequal delivery");
                    if (submitted_leave) expect(event.key == *submitted_leave, "ordinary leave retains its original agreed stamp");
                    if (humans > 2) {
                        const auto& policy = rig.games[peer]->galaxy().seat_control()->control;
                        expect(policy.teams.front().lead == rig.humans[1], "coop lead moves to the lowest remaining seat");
                        expect(policy.controllers.front().kind == galactic::FactionControl::human,
                            "a remaining coop human retains faction control");
                        checked_lead = true;
                    }
                }
                if (event.key.seat == rig.humans[peer]) inactive[peer] = true;
            }
        }
        if (agreed && !rest_leaving && rig.adapters[survivor]->completed_tick() >= 32) {
            rest_leaving = true;
            // The last survivor leaves after the other ordinary quits have reached every active peer.
            if (humans > 2)
                for (net::PeerIndex peer = 1; peer < survivor; ++peer)
                    require(rig.sessions[peer].submit([](auto key) { return net::encode_galactic_input(key, galactic::SeatLeave{}); }));
        }
        if (humans > 2 && !last_leave && rig.adapters[survivor]->completed_tick() >= 48)
            last_leave = require(rig.sessions[survivor].submit([](auto key) {
                return net::encode_galactic_input(key, galactic::SeatLeave{});
            }));
        if (inactive[survivor]) break;
        if (rig.adapters[survivor]->completed_tick() == options.final_tick && rig.monitors[survivor]->agreed_end()) break;
    }
    expect(triggered && agreed.has_value(), "mid-match loss produced an agreed departure");
    if (!agreed) return;
    expect(agreed->reason == (disconnect ? net::DepartureReason::disconnect : net::DepartureReason::quit), "agreed departure cause");
    if (humans == 2)
        expect(rig.adapters[survivor]->completed_tick() == options.final_tick && rig.monitors[survivor]->agreed_end(),
            "versus survivor continues and agrees its final digest");
    else
        expect(last_leave && rig.adapters[survivor]->completed_tick() == last_leave->tick + 1,
            "the remaining coop human can keep playing and then quit at its agreed tick");
    for (net::PeerIndex peer = 0; peer < humans; ++peer) {
        if (disconnect && peer == leaver) continue;
        expect(!events[peer].empty(), "each live peer applied a departure");
        const auto common = std::min(rig.hashes[peer].size(), rig.hashes[survivor].size());
        expect(common > agreed->key.tick && std::equal(rig.hashes[peer].begin(),
            rig.hashes[peer].begin() + static_cast<std::ptrdiff_t>(common), rig.hashes[survivor].begin()),
            "surviving digests are equal before, at and after the agreed leave tick");
        expect(std::equal(rig.inputs[peer].begin(), rig.inputs[peer].begin() + static_cast<std::ptrdiff_t>(common),
            rig.inputs[survivor].begin()), "canonical inputs agree through every common tick");
    }
    const auto& control = rig.games[survivor]->galaxy().seat_control()->control;
    if (humans == 2) {
        expect(control.controllers[1].kind == galactic::FactionControl::ai, "D5: versus continues with configured AI fallback");
        const auto seats = rig.games[survivor]->galaxy().seats();
        expect(std::any_of(seats.begin(), seats.end(), [](const auto& seat) { return seat.player == 2
            && seat.kind == eawr::sim::SeatKind::ai && seat.active; }), "versus fallback issuer is active");
    } else {
        expect(checked_lead && control.teams.front().lead.value == 0
            && control.controllers.front().kind == galactic::FactionControl::ai,
            "last coop human departure clears the lead and activates configured AI fallback");
        const auto seats = rig.games[survivor]->galaxy().seats();
        expect(std::any_of(seats.begin(), seats.end(), [](const auto& seat) { return seat.player == 1
            && seat.kind == eawr::sim::SeatKind::ai && seat.active; }), "coop fallback issuer is active");
        expect(control.controllers.front().configuration == setup.seat_control->controllers.front().configuration
            && control.controllers.front().state == setup.seat_control->controllers.front().state,
            "departure retains the configured controller and persistent state");
    }
    const auto before_rejoin = rig.adapters[survivor]->state_hash();
    const eawr::sim::SeatCommandKey rejoin_key{rig.adapters[survivor]->completed_tick(), rig.humans[leaver], 999};
    const net::SeatInput rejoin{rejoin_key, require(net::encode_galactic_input(rejoin_key, galactic::SeatRejoin{}))};
    const auto refused = rig.adapters[survivor]->validate(rejoin);
    expect(!refused && refused.error().message.find("rejoin is disabled") != std::string::npos,
        "a departed Galactic seat cannot rejoin and receives a clear diagnostic");
    expect(!rig.adapters[survivor]->install(std::array{rejoin}), "a rejoin cannot bypass admission through install");
    expect(rig.adapters[survivor]->state_hash() == before_rejoin, "refused rejoin changes no agreed state");
}

void two_peer_split_continues_separately() {
    Rig::Options options;
    options.final_tick = 80;
    options.departures = true;
    options.workers = {1, 8};
    Rig rig(policy_setup(), options);
    for (std::uint64_t round = 0; round < 40; ++round) {
        rig.network.round = round;
        for (auto& session : rig.sessions) require(session.pump(round));
    }
    rig.network.open[0] = rig.network.open[1] = false;
    for (std::uint64_t round = 40; round < 2000; ++round) {
        rig.network.round = round;
        for (net::PeerIndex peer = 0; peer < 2; ++peer) {
            const auto result = require(rig.sessions[peer].pump(round));
            for (const auto& event : result.departure_events) rig.departures[peer].push_back(event.key);
        }
        if (rig.monitors[0]->agreed_end() && rig.monitors[1]->agreed_end()) break;
    }
    for (net::PeerIndex peer = 0; peer < 2; ++peer) {
        const auto other = 1U - peer;
        expect(rig.adapters[peer]->completed_tick() == options.final_tick && rig.monitors[peer]->agreed_end(),
            "D5: each side of a two-peer link split can continue its separate campaign");
        expect(rig.departures[peer].size() == 1 && rig.departures[peer].front().seat == rig.humans[other],
            "each split campaign applies one leave for the unreachable human");
        const auto& policy = rig.games[peer]->galaxy().seat_control()->control;
        expect(policy.controllers[peer].kind == galactic::FactionControl::human
            && policy.controllers[other].kind == galactic::FactionControl::ai,
            "each split campaign retains its own human and activates the other's configured fallback");
        const eawr::sim::SeatCommandKey key{options.final_tick, rig.humans[other], 999};
        const auto rejoin = rig.adapters[peer]->validate({key, require(net::encode_galactic_input(key, galactic::SeatRejoin{}))});
        expect(!rejoin && rejoin.error().message.find("rejoin is disabled") != std::string::npos,
            "D8: split campaigns cannot reconcile by rejoining a departed seat");
    }
}

void multiple_survivors_keep_confirmation_policy() {
    Rig::Options options;
    options.final_tick = 80;
    options.departures = true;
    options.monitor = false; // isolate the departure agreement from the hash gate
    options.completeness_timeout_ms = 20;
    Rig rig(coop_policy_setup(3), options);
    for (std::uint64_t round = 0; round < 40; ++round) {
        rig.network.round = round;
        for (auto& session : rig.sessions) require(session.pump(round));
    }
    std::array<bool, 3> failed{};
    bool departed = false;
    // Peer 0 keeps an open stream but stops producing input. continue_alone must not waive the
    // existing timeout/confirmation policy when two survivors still participate in the agreement.
    for (std::uint64_t round = 40; round < 240 && !(failed[1] && failed[2]); ++round) {
        rig.network.round = round;
        for (net::PeerIndex peer = 1; peer < 3; ++peer) {
            if (failed[peer]) continue;
            const auto result = rig.sessions[peer].pump(round);
            if (!result) {
                failed[peer] = true;
                expect(result.error().code == net::session_errors::stalled,
                    "multiple-survivor timeout still reports unconfirmed connection loss");
            } else departed = departed || !result.value().departure_events.empty();
        }
    }
    expect(failed[1] && failed[2] && !departed, "continue_alone cannot bypass the existing multiple-survivor confirmation rule");
    for (net::PeerIndex peer = 1; peer < 3; ++peer) {
        const auto seats = rig.games[peer]->galaxy().seats();
        expect(seats.front().active, "an unconfirmed multi-peer timeout does not synthesize a leave");
    }
}

void recovered_membership_frontier_contracts() {
    constexpr std::uint64_t boundary = 6, final_tick = 13;
    for (const auto peers : {2U, 3U}) for (const auto workers : {1U, 2U, 4U, 8U}) {
        const auto setup = coop_policy_setup(peers);
        FakeNetwork network(peers);
        std::vector<std::unique_ptr<eawr::platform::ThreadWorkerAdapter>> executors;
        std::vector<std::unique_ptr<GameSession>> games;
        std::vector<std::unique_ptr<net::GalacticSessionAdapter>> adapters;
        std::vector<std::unique_ptr<FakeTransport>> transports;
        std::vector<std::unique_ptr<net::LockstepSession>> sessions;
        net::RecoverySessionFrontier frontier;
        frontier.completed_tick = boundary; frontier.input_delay = 3;
        frontier.mesh_incident = peers == 3 ? 7U : 0U;
        frontier.peers.resize(peers);
        for (auto& row : frontier.peers) row.next_seal = boundary;
        frontier.peers.back().departed = true; frontier.peers.back().producer_sequence = 2;
        const eawr::sim::SeatCommandKey future_key{boundary, {1}, 50};
        const auto changed_planet = setup.planets.front().id;
        const net::SeatInput future{future_key, require(net::encode_galactic_input(future_key,
            galactic::DebugSetPlanetOwner{changed_planet, 3}))};
        frontier.peers.front().next_seal = boundary + 1; frontier.peers.front().producer_sequence = 50;
        frontier.future.push_back({0, boundary, boundary, {future}});
        const eawr::sim::SeatCommandKey rejoin_key{boundary, {peers}, 3};
        const net::SeatInput rejoin{rejoin_key, require(net::encode_galactic_input(rejoin_key, galactic::SeatRejoin{}))};
        for (net::PeerIndex peer = 0; peer < peers; ++peer) {
            executors.push_back(std::make_unique<eawr::platform::ThreadWorkerAdapter>(workers));
            auto game = require(GameSession::create(setup));
            for (std::uint64_t tick = 0; tick < boundary; ++tick) {
                if (tick == boundary - 1) require(game.submit({{tick, {peers}, 2}, galactic::SeatLeave{}}));
                require(game.step(*executors.back()));
            }
            games.push_back(std::make_unique<GameSession>(std::move(game)));
            adapters.push_back(std::make_unique<net::GalacticSessionAdapter>(*games.back(), *executors.back()));
            transports.push_back(std::make_unique<FakeTransport>(network, peer));
            net::SessionOptions options; options.initial_tick = boundary; options.final_tick_count = final_tick;
            if (peers == 3) options.mesh.emplace();
            options.producer_sequences = games.back()->producer_sequences();
            std::vector<net::PeerSeat> seats;
            for (std::uint32_t id = 1; id <= peers; ++id) seats.push_back({{id}, true});
            auto session = require(net::LockstepSession::create(*adapters.back(), *transports.back(), seats, options));
            session.enable_departures([](auto key) { return net::encode_galactic_input(key, galactic::SeatLeave{}); });
            expect(!adapters.back()->validate(rejoin), "normal human wire cannot activate a departed saved seat");
            auto split = frontier; split.split = true;
            expect(!session.restore_recovery_frontier(split, rejoin), "split lineage cannot restore a frontier or reactivate its peer");
            auto wrong = rejoin; wrong.bytes = future.bytes;
            expect(!session.restore_recovery_frontier(frontier, wrong), "ordinary gameplay cannot masquerade as a recorded rejoin");
            require(session.restore_recovery_frontier(frontier, rejoin));
            expect(session.mesh_incident_id() == frontier.mesh_incident, "private recovery preserves the agreed room mesh counter");
            expect(!session.submit([](auto) { return eawr::core::Result<Bytes>::success({1}); }),
                "local admission waits for the first recorded rejoin tick");
            sessions.push_back(std::make_unique<net::LockstepSession>(std::move(session)));
        }
        bool returning_submitted{};
        for (std::uint64_t round = 0; round < 300; ++round) {
            network.round = round;
            for (auto& session : sessions) require(session->pump(round));
            if (!returning_submitted && games.back()->completed_tick() > boundary) {
                const auto key = require(sessions.back()->submit([](auto stamp) {
                    return net::encode_galactic_input(stamp, galactic::ResumeGalaxy{});
                }));
                expect(key.seat.value == peers && key.sequence == 4, "recovered input continues the saved logical seat and inclusive sequence");
                returning_submitted = true;
            }
            if (std::all_of(games.begin(), games.end(), [](const auto& game) { return game->completed_tick() == final_tick; })) break;
        }
        for (std::size_t peer = 0; peer < games.size(); ++peer) {
            expect(games[peer]->completed_tick() == final_tick && adapters[peer]->state_hash() == adapters.front()->state_hash(),
                "recovered membership and sealed future input agree through N+k on every peer and worker count");
            const auto seats = games[peer]->galaxy().seats();
            const auto returned = std::find_if(seats.begin(), seats.end(), [peers](const auto& row) { return row.seat_id.value == peers; });
            expect(returned != seats.end() && returned->active, "the one recorded transition reactivates its original saved seat");
            const auto planets = games[peer]->galaxy().snapshot()->planets();
            const auto changed = std::find_if(planets.begin(), planets.end(), [changed_planet](const auto& row) { return row.id == changed_planet; });
            expect(changed != planets.end() && changed->owner == 3, "already accepted future input survives the private frontier install");
        }
    }
}

net::RecoveryDigest recovery_digest(std::string_view text) {
    net::RecoveryDigest digest{};
    if (text.size() != 64) throw std::runtime_error("state hash is not SHA256");
    const auto nibble = [](char value) -> unsigned {
        if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a') + 10U;
        throw std::runtime_error("noncanonical hash");
    };
    for (std::size_t index = 0; index < digest.size(); ++index)
        digest[index] = static_cast<std::uint8_t>((nibble(text[index * 2]) << 4U) | nibble(text[index * 2 + 1]));
    return digest;
}
std::optional<net::GalacticRecoveryWorker::Result> wait_recovery(net::GalacticRecoveryWorker& worker,
    const net::GalacticRecoveryAnchor& barrier) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = worker.take(barrier)) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("private recovery worker did not complete");
}
void checkpoint_recovery_contracts() {
    using Refusal = net::GalacticRecoveryRefusal;
    const Bytes large_log(2 * net::save_chunk_bytes + 17, 42);
    const auto first_chunk = require(net::GalacticRecoveryStream::chunk(1, large_log, 0));
    const auto last_chunk = require(net::GalacticRecoveryStream::chunk(1, large_log, 2 * net::save_chunk_bytes));
    net::GalacticRecoveryStream ordered(1);
    expect(ordered.receive(first_chunk) == Refusal::none && ordered.receive(last_chunk) == Refusal::malformed,
        "out-of-order catch-up chunks refuse within their fixed bound");
    auto changed_chunk = first_chunk; changed_chunk.back() ^= 1;
    expect(ordered.receive(changed_chunk) == Refusal::malformed, "changed duplicate catch-up chunk refuses");
    net::GalacticRecoveryStream other_epoch(2), tiny(1, 16);
    expect(other_epoch.receive(first_chunk) == Refusal::divergent_epoch, "stale epoch chunks cannot enter a new incident");
    expect(tiny.receive(first_chunk) == Refusal::limit && tiny.progress() == 0, "oversized catch-up allocates no payload storage");
    for (const auto peers : {2U, 3U}) for (const auto workers : {1U, 2U, 4U, 8U}) {
        const auto setup = coop_policy_setup(peers);
        const auto identities = eawr::checkpoint::game_identities(setup);
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = require(GameSession::create(setup));
        net::GalacticSessionAdapter adapter(game, executor);
        for (unsigned tick = 0; tick < 7; ++tick) require(adapter.step());
        require(game.retain_network_resume_state(adapter.checkpoint_network_state()));
        auto bytes = std::make_shared<const Bytes>(require(eawr::checkpoint::save_game_checkpoint(game, identities)));
        net::GalacticRecoveryAnchor anchor{1, 7, {}, recovery_digest(adapter.state_hash())}; anchor.lineage.fill(17);
        std::vector<net::PeerSeat> seats;
        for (std::uint32_t peer = 0; peer < peers; ++peer) seats.push_back({{peer + 1}, true});
        net::GalacticRecoveryLog log(4096);
        expect(log.checkpoint(anchor, bytes, seats) == Refusal::none, "agreed checkpoint opens bounded recovery history");
        for (std::uint64_t tick = 7; tick < 19; ++tick) {
            std::vector<net::SeatInput> inputs;
            if (tick == 9 || tick == 13) {
                galactic::CommandPayload payload = tick == 9 ? galactic::CommandPayload{galactic::PauseGalaxy{}}
                    : galactic::CommandPayload{galactic::ResumeGalaxy{}};
                const eawr::sim::SeatCommandKey key{tick, {1}, tick};
                inputs.push_back({key, require(net::encode_galactic_input(key, payload))});
            }
            if (tick == 16 && peers == 3) {
                const eawr::sim::SeatCommandKey key{tick, {2}, tick};
                inputs.push_back({key, require(net::encode_galactic_input(key, galactic::SeatLeave{}))});
            }
            require(adapter.install(inputs)); require(adapter.step());
            expect(log.append(tick, inputs, recovery_digest(adapter.state_hash())) == Refusal::none,
                "sealed log retains pause windows and another peer departure");
        }
        net::GalacticRecoveryPackage package;
        expect(log.package(peers - 1, {anchor, {peers}}, package) == Refusal::none,
            "authenticated saved seat obtains the checkpoint and complete sealed suffix");
        net::GalacticRecoveryWorker::Restore restore = [identities, workers](const auto& candidate)
            -> eawr::core::Result<std::unique_ptr<net::SessionAdapter>> {
            eawr::checkpoint::GameLoadOptions options; options.script_requirement = eawr::campaign::ScriptRequirement::none;
            const eawr::sim::InlineExecutor restore_executor;
            auto restored = eawr::checkpoint::load_game_checkpoint(*candidate.bytes, identities, options, restore_executor);
            if (!restored) return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::failure(restored.error());
            auto game = std::make_shared<GameSession>(std::move(restored).value().session);
            auto executor = std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers);
            auto adapter = net::GalacticSessionAdapter::owned(std::move(game), std::move(executor));
            if (!adapter) return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::failure(adapter.error());
            return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::move(adapter).value());
        };
        const auto request_wire = require(net::encode_galactic_recovery_request({anchor, {peers}, 1}));
        const auto request = require(net::decode_galactic_recovery_request(request_wire));
        expect(request.anchor == anchor && request.seat.value == peers, "recovery request binds saved seat and exact lineage anchor");
        net::GalacticRecoveryControl vote{1, package.frontier.completed_tick, 0, peers - 1,
            net::GalacticRecoveryStage::vote, package.frontier, {}};
        vote.sealed_log.fill(5);
        const auto vote_wire = require(net::encode_galactic_recovery_control(vote));
        const auto decoded_vote = require(net::decode_galactic_recovery_control(vote_wire));
        expect(decoded_vote.transfer_epoch == 1 && decoded_vote.target == peers - 1 && decoded_vote.frontier == package.frontier
            && decoded_vote.sealed_log == vote.sealed_log, "authenticated proof binds the exact incident, saved peer and sealed frontier");
        auto wrong_vote = vote_wire; wrong_vote[25] = 255;
        expect(!net::decode_galactic_recovery_control(wrong_vote), "unknown recovery proof stages refuse");
        wrong_vote = vote_wire; wrong_vote.push_back(0);
        expect(!net::decode_galactic_recovery_control(wrong_vote), "proof suffix cannot smuggle input");
        auto request_tail = request_wire; request_tail.push_back(0);
        expect(!net::decode_galactic_recovery_request(request_tail), "hostile request suffix refuses cleanly");
        auto wire = require(net::encode_galactic_recovery_log(package));
        net::GalacticRecoveryStream incoming(anchor.epoch);
        for (std::size_t offset = 0; offset < wire.size(); offset += net::save_chunk_bytes) {
            const auto chunk = require(net::GalacticRecoveryStream::chunk(anchor.epoch, wire, offset));
            expect(incoming.receive(chunk) == Refusal::none, "bounded catch-up chunks assemble independently of gameplay");
            const auto progress = incoming.progress();
            expect(incoming.receive(chunk) == Refusal::none && incoming.progress() == progress,
                "duplicate catch-up frames do not grow memory or advance progress");
        }
        expect(incoming.complete(), "whole catch-up suffix received");
        package = require(net::decode_galactic_recovery_log(incoming.bytes(), bytes));
        auto trailing_wire = wire; trailing_wire.push_back(0);
        expect(!net::decode_galactic_recovery_log(trailing_wire, bytes), "trailing sealed-log bytes refuse");
        net::GalacticRecoveryWorker worker;
        expect(worker.start(package, restore) == Refusal::none, "private catch-up starts without touching the live owner");
        expect(worker.start(package, restore) == Refusal::busy, "a second catch-up is bounded to one worker");
        auto result = wait_recovery(worker, package.frontier);
        expect(result && result->refusal == Refusal::none && result->owner
            && result->owner->state_hash() == adapter.state_hash(), "checkpoint catch-up reproduces N+k on all worker counts");
        const auto live_hash = adapter.state_hash();
        auto wrong = anchor; ++wrong.epoch;
        expect(log.package(peers - 1, {wrong, {peers}}, package) == Refusal::divergent_epoch, "divergent epochs refuse recovery");
        wrong = anchor; wrong.lineage[0] ^= 1;
        expect(log.package(peers - 1, {wrong, {peers}}, package) == Refusal::divergent_epoch, "split descendants cannot reconcile");
        wrong = anchor; wrong.state[0] ^= 1;
        expect(log.package(peers - 1, {wrong, {peers}}, package) == Refusal::anchor_hash, "divergent anchor hashes refuse recovery");
        expect(log.package(peers - 1, {anchor, {1}}, package) == Refusal::seat_binding, "a replacement cannot claim another saved seat");
        expect(log.package(peers - 1, {anchor, {peers}}, package) == Refusal::none, "refusals leave the valid recovery usable");
        auto hostile = package; hostile.ticks[1].tick = hostile.ticks[0].tick;
        expect(worker.start(std::move(hostile), restore) == Refusal::none, "bounded hostile log enters only private staging");
        result = wait_recovery(worker, package.frontier);
        expect(result && result->refusal == Refusal::malformed && !result->owner, "duplicate/out-of-order catch-up rows cannot publish");
        auto barrier = package.frontier; ++barrier.completed_tick;
        expect(worker.start(package, restore) == Refusal::none, "a fresh catch-up can follow a hostile candidate");
        result = wait_recovery(worker, barrier);
        expect(result && result->refusal == Refusal::stale_barrier && !result->owner, "a stale catch-up cannot replace a later owner");
        expect(adapter.state_hash() == live_hash, "every refusal preserves the running match owner");
        net::GalacticRecoveryLog short_log(44);
        expect(short_log.checkpoint(anchor, bytes, seats) == Refusal::none, "small log accepts checkpoint independently of byte transfer");
        for (const auto& row : package.ticks) static_cast<void>(short_log.append(row.tick, row.inputs, row.state));
        expect(short_log.retained_bytes() <= 44 && short_log.package(peers - 1, {anchor, {peers}}, hostile) == Refusal::log_too_old,
            "expired recovery refuses by name within its fixed memory budget");
        log.invalidate();
        expect(log.package(peers - 1, {anchor, {peers}}, hostile) == Refusal::unavailable,
            "an unconfirmed split invalidates the shared checkpoint");
        std::cout << "checkpoint recovery peers=" << peers << " workers=" << workers << " state=" << live_hash << '\n';
    }
}

} // namespace

// #5519: the lockstep exchange encoding feeds the desync bundle's session.bin. One peer receives a pause no other peer saw;
// both peers' bundles must then hold the byte-identical SERIAL encoding of an agreed exchange tick: the earlier baseline
// when the divergence starts in the first half of the exchange window, the newest tick otherwise. Run with one worker
// (serial exchange encode) and four (sections encoded concurrently).
void forced_mismatch_bundles() {
    for (const std::size_t workers : {1U, 4U}) {
        for (const std::uint64_t divergence : {12U, 17U}) {
            const std::string where = "forced mismatch workers=" + std::to_string(workers) + " at tick " + std::to_string(divergence) + ": ";
            Rig::Options options;
            options.final_tick = 200;
            options.workers = {workers};
            Rig rig(versus_setup(), options);
            std::array<std::map<std::uint64_t, Bytes>, 2> serial; // oracle: fresh serial galaxy encoding at each exchange tick
            bool injected = false;
            // The deliberately inconsistent engine source of one peer: an AI-seat ownership change the other never sees.
            const auto planet = rig.setup.planets[5 % rig.setup.planets.size()].id;
            rig.adapters[1]->set_engine_source([&](const GameSession&, std::uint64_t tick) {
                std::vector<galactic::GameCommand> produced;
                if (tick == divergence) {
                    injected = true;
                    produced.push_back({{tick, {3}, 9000}, galactic::DebugSetPlanetOwner{planet, 3}});
                }
                return eawr::core::Result<std::vector<galactic::GameCommand>>::success(std::move(produced));
            });
            rig.run(6000, [&](const std::uint64_t) {
                for (net::PeerIndex peer = 0; peer < 2; ++peer) {
                    const auto tick = rig.adapters[peer]->completed_tick();
                    if (tick % 10 == 0 && !serial[peer].contains(tick))
                        serial[peer].emplace(tick, rig.games[peer]->galaxy().canonical_state_bytes());
                }
            });
            expect(injected, where + "the one-peer command was injected");
            for (net::PeerIndex peer = 0; peer < 2; ++peer) {
                expect(rig.monitors[peer]->report().has_value(), where + "every peer holds a desync report"
                    + (rig.failure ? " (session failure " + rig.failure->code + ": " + rig.failure->message + ")" : std::string()));
                if (!rig.monitors[peer]->report()) continue;
                const auto& report = *rig.monitors[peer]->report();
                const auto file = std::find_if(report.bundle.files.begin(), report.bundle.files.end(),
                    [](const auto& entry) { return entry.name == "session.bin"; });
                expect(file != report.bundle.files.end() && !file->bytes.empty(), where + "the bundle holds a session encoding");
                if (file == report.bundle.files.end()) continue;
                expect(report.encoding_tick % 10 == 0 && serial[peer].contains(report.encoding_tick),
                    where + "the retained encoding is an agreed exchange tick");
                if (!serial[peer].contains(report.encoding_tick)) continue;
                expect(file->bytes == serial[peer].at(report.encoding_tick),
                    where + "session.bin equals the fresh serial encoding of its tick, byte for byte");
                if (divergence < 15) expect(report.encoding_tick < report.detected_at, where + "an early divergence keeps the earlier baseline");
                else expect(report.encoding_tick == report.detected_at, where + "a late divergence keeps the newest encoding");
            }
        }
    }
}

// A phase that always fails: the exchange encode must discard partial buffers and fall back to the serial encode.
class FailingExecutor final : public eawr::sim::PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 4; }
    [[nodiscard]] eawr::core::Result<void> execute(std::size_t, const std::function<void(std::size_t)>&) const override {
        eawr::core::Diagnostic error;
        error.code = "EAWR-TEST-EXECUTOR";
        error.message = "forced phase failure";
        return eawr::core::Result<void>::failure(std::move(error));
    }
};

// #5519: the exchange encoding is the serial canonical encoding for every worker count, also when the worker phase fails.
void exchange_encode_equals_serial() {
    auto game = require(GameSession::create(versus_setup()));
    const eawr::platform::ThreadWorkerAdapter stepping(1);
    const FailingExecutor failing;
    std::size_t compared = 0;
    for (std::uint64_t tick = 0; tick < 45; ++tick) {
        static_cast<void>(require(game.step(stepping)));
        if (game.completed_tick() % 10 != 0) continue;
        const auto serial = game.galaxy().canonical_state_bytes();
        for (const std::size_t workers : {1U, 2U, 4U, 8U, 10U}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(game.galaxy().exchange_state_bytes(executor) == serial,
                "exchange encoding equals the serial encoding at " + std::to_string(workers) + " workers, tick " + std::to_string(tick));
        }
        expect(game.galaxy().exchange_state_bytes(failing) == serial, "a failed worker phase falls back to the serial encoding");
        ++compared;
    }
    expect(compared >= 4, "several exchange ticks were compared");
}

// #5519: a loaded checkpoint resets the exchange offer cache and scratch buffers; the next exchange encode rebuilds them
// inside a parallel task and still equals the serial encoding (and the donor's).
void exchange_encode_after_checkpoint_load() {
    const eawr::platform::ThreadWorkerAdapter stepping(1);
    auto donor = require(GameSession::create(versus_setup()));
    for (std::uint64_t tick = 0; tick < 20; ++tick) static_cast<void>(require(donor.step(stepping)));
    const auto saved = require(donor.save_checkpoint_state());
    const auto donor_serial = donor.galaxy().canonical_state_bytes();
    for (const std::size_t workers : {1U, 4U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto target = require(GameSession::create(versus_setup()));
        // Prime the cache and scratch on the fresh game (restore needs a fresh private game), so a stale cache would show after the load.
        expect(target.galaxy().exchange_state_bytes(executor) == target.galaxy().canonical_state_bytes(),
            "exchange equals serial before the load at " + std::to_string(workers) + " workers");
        require(target.load_checkpoint_state(saved));
        const auto serial = target.galaxy().canonical_state_bytes();
        expect(serial == donor_serial, "the loaded state encodes like the donor at " + std::to_string(workers) + " workers");
        expect(target.galaxy().exchange_state_bytes(executor) == serial,
            "exchange equals serial after the load at " + std::to_string(workers) + " workers");
        static_cast<void>(require(target.step(stepping)));
        expect(target.galaxy().exchange_state_bytes(executor) == target.galaxy().canonical_state_bytes(),
            "exchange equals serial one tick after the load at " + std::to_string(workers) + " workers");
    }
}

// #5355: a multiplayer save's clock continuation written under the older shared-clock constants (6 speed changes per
// 1,350 session ticks, #4574) still resumes. Its speed history is dropped; policy, pause actor and debounce stay, and the
// adapter writes the current constants from then on. Malformed or over-long rows stay refused.
void legacy_clock_continuation() {
    const auto put = [](Bytes& bytes, std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
    };
    struct Row { std::uint32_t seat; std::vector<std::uint64_t> ticks; };
    const auto continuation = [&](std::uint64_t window, std::uint64_t limit, const std::vector<Row>& history,
                                  std::uint32_t toggle_seat, std::uint64_t toggle_tick) {
        Bytes bytes;
        put(bytes, static_cast<std::uint8_t>(net::GalacticClockPolicy::limited), 1);
        put(bytes, window, 8); put(bytes, limit, 4);
        put(bytes, 1, 1); put(bytes, 1, 4); // pause actor: seat 1
        put(bytes, history.size(), 4);
        for (const auto& row : history) {
            put(bytes, row.seat, 4); put(bytes, row.ticks.size(), 4);
            for (const auto tick : row.ticks) put(bytes, tick, 8);
        }
        put(bytes, 1, 4); put(bytes, toggle_seat, 4); put(bytes, toggle_tick, 8);
        return bytes;
    };
    const auto setup = coop_policy_setup(2);
    const eawr::platform::ThreadWorkerAdapter executor(1);
    auto game = require(GameSession::create(setup));
    net::GalacticAdapterOptions options; options.clock_policy = net::GalacticClockPolicy::limited;
    {
        net::GalacticSessionAdapter stepping(game, executor, options);
        for (unsigned tick = 0; tick < 10; ++tick) require(stepping.step());
    }
    const auto current = continuation(net::ClockThrottle::window_ticks, net::ClockThrottle::speed_changes_per_window, {}, 1, 5);
    const auto old = continuation(1350, 6, {{1, {2, 3, 4}}, {2, {6}}}, 1, 5);
    net::GalacticSessionAdapter resumed(game, executor, options);
    expect(bool(resumed.restore_network_state(old)), "a continuation saved under the 6 per 1,350-tick budget resumes");
    expect(resumed.checkpoint_network_state() == current,
        "the old speed history is dropped; policy, pause actor and debounce are kept under the current constants");
    net::GalacticSessionAdapter same(game, executor, options);
    expect(bool(same.restore_network_state(current)) && same.checkpoint_network_state() == current,
        "a current continuation still round-trips byte for byte");
    net::GalacticSessionAdapter refused(game, executor, options);
    expect(!refused.restore_network_state(continuation(1350, 2, {{1, {2, 3, 4}}}, 1, 5)),
        "a row longer than its own saved limit is refused");
    expect(!refused.restore_network_state(continuation(net::ClockThrottle::window_ticks, net::ClockThrottle::speed_changes_per_window,
        {{1, {2, 3, 4}}}, 1, 5)), "a current-constant row longer than the current limit is refused");
    expect(!refused.restore_network_state(continuation(0, 6, {}, 1, 5)), "a zero window is refused");
    expect(!refused.restore_network_state(continuation(1350, 6, {{1, {2, 30}}}, 1, 5)),
        "an old row with a tick at or after the saved frontier is refused");
    auto trailing = old; trailing.push_back(0);
    expect(!refused.restore_network_state(trailing), "trailing bytes are refused");
}

void long_campaign_session_and_save() {
    namespace checkpoint = eawr::checkpoint;
    constexpr std::uint64_t start = net::max_session_ticks - 2, end = net::max_session_ticks + 6;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        Rig::Options options; options.workers = {workers}; options.initial_tick = start; options.final_tick = end;
        Rig rig(versus_setup(), options);
        rig.run();
        expect(!rig.failure && rig.games[0]->completed_tick() == end && rig.games[1]->completed_tick() == end,
            "both GC peers cross the tactical tick cap and reach the agreed end");
        expect(rig.hashes[0] == rig.hashes[1] && rig.hashes[0].size() == end - start,
            "long-session peers exchange matching hashes for every completed tick");
        if (reference.empty()) reference = rig.hashes[0];
        else expect(reference == rig.hashes[0], "long-session hashes match on 1/2/4/8 workers");
        const auto identities = checkpoint::game_identities(rig.setup);
        const auto bytes = require(checkpoint::save_game_checkpoint(*rig.games[0], identities));
        const auto parsed = require(checkpoint::parse_checkpoint(bytes));
        expect(parsed.header.completed_tick == end, "saved checkpoint retains the tick beyond one million");
        static_assert(sizeof(decltype(net::SealedBatch::tick)) == 8);
        static_assert(sizeof(decltype(net::RecoverySessionFrontier::completed_tick)) == 8);
        static_assert(sizeof(decltype(net::GalacticRecoveryAnchor::completed_tick)) == 8);
        static_assert(sizeof(decltype(checkpoint::Header::completed_tick)) == 8);
        net::GalacticRecoveryAnchor anchor; anchor.epoch = 1; anchor.completed_tick = end;
        anchor.lineage.fill(1); anchor.state = recovery_digest(rig.adapters[0]->state_hash());
        net::GalacticRecoveryRequest request; request.anchor = anchor; request.seat = {1}; request.transfer_epoch = 1;
        expect(require(net::decode_galactic_recovery_request(require(net::encode_galactic_recovery_request(request)))).anchor == anchor,
            "long checkpoint recovery request retains its full tick");
        net::GalacticRecoveryControl control; control.transfer_epoch = 1; control.barrier = end;
        control.target = 1; control.frontier = anchor;
        expect(require(net::decode_galactic_recovery_control(require(net::encode_galactic_recovery_control(control)))).barrier == end,
            "long recovery control retains its publication barrier");
        net::GalacticRecoveryLog log;
        expect(log.checkpoint(anchor, std::make_shared<const Bytes>(bytes), {{{1}, true}, {{2}, true}})
            == net::GalacticRecoveryRefusal::none, "long checkpoint anchors the bounded recovery log");
        expect(log.append(end, {}, anchor.state) == net::GalacticRecoveryRefusal::none,
            "recovery log appends beyond the tactical tick cap");
        auto restored = require(checkpoint::load_game_checkpoint(bytes, identities, {}, *rig.executors[0]));
        expect(restored.work.resimulated_ticks == 0 && restored.session.completed_tick() == end
            && restored.session.state_sha256() == rig.games[0]->state_sha256(),
            "default campaign restore accepts the long session and restores the exact hash without historical work");
        expect(require(restored.session.step(*rig.executors[0])).state_hash.get()
            == require(rig.games[0]->step(*rig.executors[0])).state_hash.get(),
            "restored long session continues with the same next hash");
        checkpoint::GameLoadOptions bounded; bounded.max_restore_ticks = net::max_session_ticks;
        expect(!checkpoint::load_game_checkpoint(bytes, identities, bounded, *rig.executors[0]),
            "an explicit smaller restore budget remains enforced");
    }
}

int main() {
    try {
        long_campaign_session_and_save();
        legacy_clock_continuation();
        exchange_encode_equals_serial();
        exchange_encode_after_checkpoint_load();
        forced_mismatch_bundles();
        recovered_membership_frontier_contracts();
        checkpoint_recovery_contracts();
        classification_and_codec();
        child_hash_and_execution_refusal();
        unavailable_battle_callback_coop();
        loopback("versus", versus_setup(), std::nullopt);
        loopback("versus-wan", versus_setup(), eawr::testing::wan_profiles[1]);
        loopback("coop-4-seats", coop_setup(), std::nullopt);
        shared_clock_limit_at_execution();
        pause_progress_and_no_local_ticks();
        held_and_duplicate_transport();
        racing_inputs_are_canonical();
        refusals_through_the_session();
        engine_input_runs_on_every_peer();
        departure_through_the_session();
        for (const auto humans : {2U, 3U, 4U})
            for (const auto disconnect : {false, true}) agreed_departure_policy(humans, disconnect);
        two_peer_split_continues_separately();
        multiple_survivors_keep_confirmation_policy();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}
