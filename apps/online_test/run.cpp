#include "run.hpp"
#include "log.hpp"
#include "setup_test_support.hpp"
#include "online_match.hpp"
#include "online_service_settings.hpp"
#include "eawr/net/gns_transport.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <thread>

using namespace setup_test;
namespace viewer = eawr::presentation::godot_backend;
using namespace std::chrono_literals;

namespace eawr::online_test {
namespace {
constexpr std::uint64_t target_ticks = 10;
// The peer may still need our last inputs; keep stepping this long after tick 10.
constexpr auto linger = 8s;

std::string route_text(net::GnsRoute route) {
    switch (route) {
    case net::GnsRoute::direct: return "direct";
    case net::GnsRoute::relayed: return "relayed via TURN";
    default: return "unknown";
    }
}

struct Failure { std::string code; std::string text; };

tactical::TacticalSetup initial_setup() {
    auto initial = setup();
    initial.queue_identities = true;
    initial.departure_policy.emplace();
    initial.departure_policy->humans = {1, 2};
    for (auto& row : initial.skirmish->slots) row.fleet.clear();
    for (std::size_t i = 0; i < 3; ++i)
        initial.players[i].faction_id = eawr::skirmish::faction_id(std::array{"Rebel", "Empire", "Underworld"}[i]);
    return initial;
}

viewer::LanLobbyBuilder make_builder(const tactical::TacticalSetup& initial) {
    return [initial](const auto& choices) {
        auto built = initial;
        built.seed = choices.seed.value_or(initial.seed);
        built.skirmish->match = *choices.match;
        const tactical::SkirmishMatchPolicy policy{choices.match->allow_heroes, choices.match->allow_superweapons,
            choices.match->free_starting_units, choices.match->pre_built_base};
        if (!policy.allow_heroes || !policy.allow_superweapons || !policy.free_starting_units || !policy.pre_built_base)
            built.match_policy = policy;
        else built.match_policy.reset();
        for (const auto& slot : *choices.slots) {
            built.players[slot.slot - 1].faction_id = eawr::skirmish::faction_id(slot.faction);
            built.players[slot.slot - 1].team_id = slot.team;
            built.skirmish->slots[slot.slot - 1].colour_index = slot.colour_index;
            built.skirmish->slots[slot.slot - 1].human = slot.human;
        }
        return eawr::core::Result<tactical::TacticalSetup>::success(std::move(built));
    };
}

// Throws Failure; the caller turns it into the FAIL line.
int play(const Options& options, std::istream& in, std::ostream& out, Log& log) {
    const bool host = options.role == Role::host;
    viewer::OnlineMatchOptions match_options;
    match_options.service.address = options.server.address;
    match_options.service.port = options.server.port;
    match_options.service.certificate_name = options.server.certificate_name;
    match_options.service.certificate_sha256 = options.server.certificate_sha256;
    match_options.service.ca_file = options.server.ca_file;
    match_options.identity = host ? "online-test-host" : "online-test-guest";
    match_options.short_invitation = true;
    match_options.require_short_code = true; // the guest joins by the code alone: a service without room codes fails the host at once
    match_options.turn_only = options.turn_only;
    match_options.hash_exchange_every = target_ticks;
    match_options.turn_credentials_observer = [&log](const net::GnsTurnCredentials& credentials) {
        log.add_secret(credentials.uri);
        log.add_secret(credentials.username);
        log.add_secret(credentials.password);
    };

    std::string code = options.code;
    if (!host && code.empty()) {
        out << "Enter the 4-character code shown on the host: " << std::flush;
        std::string typed;
        std::getline(in, typed);
        code = normalise_invitation_code(typed);
        if (!valid_invitation_code(code)) throw Failure{"", "That is not a 4-character code."};
    }
    if (!code.empty()) log.add_secret(code);

    // The host publishes a plain directory room for the two humans and opens its signalling scope; the 4-character code is the
    // room's alias. The guest attaches with that code alone.
    std::shared_ptr<net::OnlineRoomSource> rooms;
    // The source's worker must be gone before the process exits, also when play() unwinds with a Failure.
    struct RoomCloser { std::shared_ptr<net::OnlineRoomSource>& source; ~RoomCloser() { if (source) source->close(); } } closer{rooms};
    if (host) {
        rooms = viewer::host_standalone_room(match_options, "Online test", 2);
        if (!rooms) throw Failure{"", "The room could not be configured."};
    }
    auto match = viewer::make_online_match(match_options);
    match->configure_factions({"Rebel", "Empire", "Underworld"});
    const auto initial = initial_setup();
    eawr::skirmish::FixtureOptions selected;
    selected.map = initial.skirmish->map;
    selected.match = initial.skirmish->match;
    selected.seed = initial.seed;
    selected.victory_condition = tactical::VictoryCondition::enemy_starbase_destroyed;
    selected.slots = std::vector<eawr::skirmish::LobbySlot>{
        {1, "Rebel", 1, true, {}, 4}, {2, "Empire", 2, true, {}, 5}, {3, "Underworld", 3, false, {}, 1}};
    eawr::skirmish::SetupMap map;
    map.path = *selected.map;
    map.metadata.capacity = 3;
    map.metadata.new_markers = true;
    map.metadata.start_positions.emplace(4);
    map.teams = {{1, {"Rebel"}, 1}, {2, {"Empire"}, 1}, {3, {"Underworld"}, 1}};
    const auto builder = make_builder(initial);
    match->configure_lobby(host ? "Host" : "Guest", host ? 4 : 5, {map}, builder);

    auto deadline = std::chrono::steady_clock::now() + options.timeout;
    net::GnsRoute route{};
    const auto pause = [&](const char* stage) {
        const auto view = match->view();
        if (view.online_route != net::GnsRoute::unknown) route = view.online_route;
        if (view.phase == viewer::LanPhase::ended)
            throw Failure{view.diagnostic_code, std::string("The session ended during ") + stage + "."};
        if (std::chrono::steady_clock::now() >= deadline)
            throw Failure{view.diagnostic_code, std::string("Timed out during ") + stage + "."};
        std::this_thread::sleep_for(2ms);
    };

    log.line(std::string("role=") + (host ? "host" : "join") + " service="
        + (options.server.certificate_sha256.empty() ? "system-ca" : "pin") + (options.turn_only ? " turn-only" : ""));
    if (host) match->host(0);
    else { out << log.line("Joining..."); out << '\n'; match->join(code); }

    bool announced{}, ready_requested{};
    std::uint64_t ready_revision{};
    log.line("stage=connect");
    while (true) {
        match->pump_setup(selected);
        const auto view = match->view();
        if (view.phase == viewer::LanPhase::ended)
            throw Failure{view.diagnostic_code, "The online session ended before the match started."};
        if (host && !announced && !view.invitation_code.empty()) {
            // The code is the only secret shown; it goes to the console and never to the log.
            code = view.invitation_code;
            log.add_secret(code);
            out << "\n  Invitation code: " << code << "\n  Tell the other player this code, then wait.\n\n" << std::flush;
            announced = true;
            log.line("stage=waiting-for-guest");
            deadline = std::chrono::steady_clock::now() + options.timeout;
        }
        const auto state = match->lobby_state();
        if (state && ready_requested && !state->preparing && !state->local_ready && state->snapshot.revision != ready_revision)
            ready_requested = false;
        if (state && !ready_requested && state->snapshot.phase == net::LobbyPhase::staging
            && state->snapshot.seats.size() == 2
            && state->snapshot.seats.front().selection.name == "Host" && state->snapshot.seats.back().selection.name == "Guest"
            && !state->identity_pending && !state->local_ready && !state->preparing) {
            log.line("stage=staging-ready");
            ready_requested = match->lobby_ready(true);
            ready_revision = state->snapshot.revision;
        }
        if (host && view.start_enabled) match->start(selected);
        if (auto chosen = match->take_start()) { selected = std::move(*chosen); break; }
        pause("connection and lobby");
    }
    log.line("stage=match-start");
    out << log.line("Connected. Playing 10 lockstep ticks...") << '\n';
    deadline = std::chrono::steady_clock::now() + options.timeout;

    auto canonical = require(builder(selected));
    auto world = require(tactical::TacticalSession::create(canonical, content()));
    sim::InlineExecutor executor;
    eawr::platform::LiveStepContext context;
    context.advance_allowed = true;
    context.completed_tick = [&] { return world.completed_tick(); };
    context.state_hash = [&] { return world.state_sha256(); };
    context.canonical_state = [&] { return world.canonical_state_bytes(); };
    context.install = [&](std::span<const tactical::PlayerCommand> commands) {
        for (const auto& command : commands) require(world.submit(command));
        return eawr::core::Result<void>::success();
    };
    context.step = [&] { require(world.step(executor)); return eawr::core::Result<void>::success(); };
    context.snapshot = [&] { return world.snapshot(); };
    auto driver = match->driver(canonical);
    const auto clock = [] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    while (world.completed_tick() < target_ticks) {
        require(driver(context, {}, clock()));
        pause("the lockstep ticks");
        if (match->view().desync_tick) throw Failure{"ONLINE-TEST-DESYNC", "The two peers' states diverged."};
    }
    const auto hash = world.state_sha256();
    log.line("stage=tick" + std::to_string(target_ticks) + " hash=" + hash);
    // Stop advancing at exactly tick 10, but continue servicing the real hash exchange.
    context.advance_allowed = false;
    const auto confirmed = [&] { return match->view().peer_confirmed_tick == target_ticks; };
    while (!confirmed()) {
        const auto driven = driver(context, {}, clock());
        const auto view = match->view();
        if (view.desync_tick) throw Failure{"ONLINE-TEST-DESYNC", "The two peers' states diverged."};
        if (!driven) throw Failure{"ONLINE-TEST-HASH-UNCONFIRMED", driven.error().code + ": " + driven.error().message
            + "; no peer-confirmed tick-10 hash was received."};
        if (!confirmed() && view.phase == viewer::LanPhase::ended)
            throw Failure{"ONLINE-TEST-HASH-UNCONFIRMED", (view.diagnostic_code.empty() ? "ONLINE-TEST-SESSION-ENDED" : view.diagnostic_code)
                + ": The session ended; no peer-confirmed tick-10 hash was received."};
        if (!confirmed() && std::chrono::steady_clock::now() >= deadline)
            throw Failure{"ONLINE-TEST-HASH-UNCONFIRMED", "Timed out waiting for a peer-confirmed tick-10 hash."};
        if (const auto seen = match->view().online_route; seen != net::GnsRoute::unknown) route = seen;
        std::this_thread::sleep_for(2ms);
    }
    if (const auto seen = match->view().online_route; seen != net::GnsRoute::unknown) route = seen;
    // Our confirmation must reach the other peer before teardown; do not advance further.
    for (const auto until = std::chrono::steady_clock::now() + linger; std::chrono::steady_clock::now() < until;) {
        if (!driver(context, {}, clock())) break;
        if (match->view().desync_tick) throw Failure{"ONLINE-TEST-DESYNC", "The two peers' states diverged."};
        std::this_thread::sleep_for(2ms);
    }
    driver = {};
    match->cancel();
    viewer::shutdown_lan_drains();
    out << "Tick-" << target_ticks << " state hash: " << hash.substr(0, 16) << " (confirmed by the peer)\n";
    out << log.line("Route: " + route_text(route)) << '\n';
    return exit_pass;
}
} // namespace

int run(const Options& options, std::istream& in, std::ostream& out) {
    Log log(options.log_path);
    log.add_secret(options.server.address);
    log.add_secret(options.server.certificate_sha256);
    log.add_secret(options.server.certificate_name);
    if (!log.ok()) out << "Warning: cannot write the log file.\n";
    if (!net::GnsTransport::available() || !net::NetworkGnsIntroduction::available()) {
        out << "RESULT: FAIL - this build has no online networking.\n";
        log.line("result=FAIL reason=no-gns");
        return exit_unavailable;
    }
    int code = exit_fail;
    try {
        code = play(options, in, out, log);
    } catch (const Failure& failure) {
        out << log.line("RESULT: FAIL - " + failure.text + (failure.code.empty() ? "" : " (" + failure.code + ")")) << '\n';
        viewer::shutdown_lan_drains();
        return exit_fail;
    } catch (const std::exception& error) {
        out << log.line(std::string("RESULT: FAIL - ") + error.what()) << '\n';
        viewer::shutdown_lan_drains();
        return exit_fail;
    }
    out << log.line("RESULT: PASS") << '\n';
    out << "Log: " << options.log_path.filename().string() << " (next to this program, safe to send)\n";
    return code;
}
}
