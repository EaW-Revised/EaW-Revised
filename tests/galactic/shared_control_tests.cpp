// FT-21 / ADR-020: two human seats share one tactical faction. The router is
// exercised through the shared lockstep transport, including held batches.
#include "../support/lockstep_loopback.hpp"
#include "eawr/sim/tactical/seat_router.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/scene/scene.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
namespace sim = eawr::sim;
namespace tactical = sim::tactical;
namespace lockstep = eawr::testing::lockstep;
namespace skirmish = eawr::skirmish;
using sim::math::Fixed;

int failures{};
void expect(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
template <typename T> T require(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
Fixed units(std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }
sim::math::Vec3 at(std::int64_t x, std::int64_t y) { return {units(x), units(y), {}}; }
const std::vector<tactical::SeatBinding> bindings{{{1}, 1, true, false}, {{2}, 1, true, false}};

struct Scenario {
    tactical::TacticalSetup setup;
    tactical::TacticalContent content;
    sim::EntityId ship{}, enemy{}, station{};
    tactical::TypeId purchase{};
    std::uint64_t build_frames{};
    sim::math::Vec3 position{};
};

Scenario synthetic() {
    Scenario result;
    result.setup.seed = 21;
    result.setup.players = {{1, 0, 1, tactical::player_flag_commandable}, {2, 1, 2, tactical::player_flag_commandable}};
    result.setup.units = {{1, 10, 1, at(-3000, 0)}, {2, 20, 1, at(-2500, 0)}, {3, 20, 2, at(3000, 0)}};
    result.ship = 2; result.enemy = 3; result.station = 1; result.purchase = 20; result.build_frames = 2;
    result.position = at(-2500, 0);
    result.content.sensors = {{10, units(20000)}, {20, units(20000)}};
    result.content.motion.rules = {units(15), units(300)};
    result.content.motion.profiles = {{20, units(4), units(1), units(1), units(6), units(1)}};
    tactical::AbilityProfile turbo;
    turbo.kind = tactical::AbilityKind::turbo;
    turbo.expiration_frames = 30; turbo.recharge_frames = 60;
    turbo.modifiers.speed = units(2);
    result.content.abilities.profiles = {{20, {turbo}}};
    result.content.abilities.humans = {1};
    result.content.economy.players = {{1, units(6000), 25, false, {}}, {2, units(6000), 25, false, {}}};
    result.content.economy.menus = {{10, 1, {{20, tactical::BuildKind::unit, tactical::BuildQueue::units,
        units(500), 2, 2, 1, true}}}};
    result.content.economy.bounds = std::array<Fixed, 4>{units(-6500), units(-6500), units(6500), units(6500)};
    return result;
}

Scenario installed(const std::filesystem::path& root) {
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        specs.push_back(require(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
    auto filesystem = require(eawr::vfs::Vfs::mount(specs));
    auto catalog = require(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    eawr::scene::VfsAssetCache cache(filesystem);
    eawr::units::LoadInput input;
    input.catalog = &catalog.catalog; input.filesystem = &filesystem; input.model = cache.access().model;
    auto tables = require(eawr::units::load_unit_tables(input));
    const auto& fixture = skirmish::m2_fixture();
    auto inputs = require(skirmish::read_start_inputs(fixture, filesystem, catalog.catalog, tables));
    auto start = require(skirmish::build_start(fixture, inputs));
    auto content = require(skirmish::session_content(tables, skirmish::human_slots(fixture)));
    Scenario result;
    result.setup = start.setup;
    result.content = {content.sensors, content.durability, content.motion, require(skirmish::fog_rules(inputs)),
        content.combat, skirmish::victory_rules(start, tables), content.abilities,
        require(skirmish::economy_rules(start, inputs, tables)), {}, {}};
    result.purchase = skirmish::type_id("Rebel_X-Wing_Squadron");
    for (const auto& unit : start.units) {
        if (unit.state.owner == 1 && unit.role == skirmish::UnitRole::station) result.station = unit.state.entity_id;
        if (unit.state.owner == 1 && unit.state.type_id == skirmish::type_id("Corellian_Corvette")) {
            result.ship = unit.state.entity_id; result.position = unit.state.position;
        }
        if (unit.state.owner == 2 && unit.state.type_id == skirmish::type_id("Tartan_Patrol_Cruiser"))
            result.enemy = unit.state.entity_id;
    }
    auto probe = require(tactical::TacticalSession::create(result.setup, result.content));
    const auto* option = probe.build_option(1, result.station, result.purchase);
    if (!result.ship || !result.enemy || !result.station) throw std::runtime_error("M2 smoke units unavailable");
    if (!option || !probe.build_allowed(1, result.station, result.purchase))
        throw std::runtime_error("M2 starting station cannot build the smoke purchase");
    result.build_frames = option->build_frames;
    return result;
}

std::vector<tactical::SeatCommand> script(const Scenario& scenario) {
    auto left = scenario.position; left.y = require(sim::math::add(left.y, units(600)));
    auto right = scenario.position; right.y = require(sim::math::subtract(right.y, units(600)));
    const auto deploy = 10 + scenario.build_frames * 2;
    return {{{0, {1}, 0}, 1, {scenario.ship}, tactical::MovePayload{left}},
        {{0, {2}, 0}, 1, {scenario.ship}, tactical::MovePayload{right}},
        {{1, {1}, 1}, 1, {scenario.ship}, tactical::StopPayload{}},
        {{1, {2}, 1}, 1, {scenario.ship}, tactical::AttackPayload{scenario.enemy}},
        {{2, {1}, 2}, 1, {scenario.ship}, tactical::AbilityPayload{tactical::AbilityKind::turbo}},
        {{3, {2}, 2}, 1, {scenario.ship}, tactical::AbilityPayload{tactical::AbilityKind::turbo, tactical::AbilityAction::deactivate}},
        {{5, {1}, 3}, 1, {scenario.station}, tactical::BuyPayload{scenario.purchase}},
        {{6, {2}, 3}, 1, {scenario.station}, tactical::BuyPayload{scenario.purchase}},
        {{deploy, {1}, 4}, 1, {}, tactical::ReinforcePayload{scenario.purchase, left}},
        {{deploy, {2}, 4}, 1, {}, tactical::ReinforcePayload{scenario.purchase, right}}};
}

struct SharedSession {
    tactical::TacticalSession world;
    tactical::SeatRouter router;
    std::vector<tactical::SeatCommand> pending;
    std::vector<tactical::AdmittedSeatCommand> provenance;
    std::vector<std::string> hashes;
    std::vector<tactical::Event> events;
};
struct SharedAdapter {
    using Session = SharedSession;
    using Command = tactical::SeatCommand;
    static std::uint64_t tick(const Command& command) { return command.key.tick; }
    static std::uint64_t issuer(const Command& command) { return command.key.seat.value; }
    static std::uint64_t sequence(const Command& command) { return command.key.sequence; }
    static eawr::core::Result<void> submit(Session& session, const Command& command) {
        session.pending.push_back(command);
        return eawr::core::Result<void>::success();
    }
    static eawr::core::Result<std::string> step(Session& session, const sim::PartitionExecutor& executor) {
        auto routed = session.router.route(session.world.completed_tick(), session.pending);
        if (!routed) return eawr::core::Result<std::string>::failure(routed.error());
        for (const auto& row : routed.value()) {
            if (auto result = session.world.submit(row.command); !result)
                return eawr::core::Result<std::string>::failure(result.error());
            session.provenance.push_back(row);
        }
        const auto submitted_tick = session.world.completed_tick();
        sim::EntityId commanded_ship{};
        for (const auto& row : routed.value()) if (!row.command.units.empty()) commanded_ship = row.command.units.front();
        session.pending.clear();
        auto result = session.world.step(executor);
        if (!result) return eawr::core::Result<std::string>::failure(result.error());
        expect(result.value().diagnostics.empty(), "all shared-control orders execute without rejection");
        if (submitted_tick <= 3) {
            const auto live = session.world.units();
            const auto ship = std::find_if(live.begin(), live.end(), [&](const auto& unit) { return unit.entity_id == commanded_ship; });
            expect(ship != live.end(), "ordered ship remains live");
            if (ship != live.end() && submitted_tick == 0)
                expect(ship->order.destination == std::get<tactical::MovePayload>(routed.value().back().command.payload).destination,
                    "higher seat's move wins on the actual unit");
            if (ship != live.end() && submitted_tick == 1)
                expect(ship->order.kind == tactical::OrderKind::attack
                    && ship->order.target == std::get<tactical::AttackPayload>(routed.value().back().command.payload).target,
                    "higher seat's attack wins over the stop");
            if (submitted_tick == 2 || submitted_tick == 3) {
                for (const auto& view : result.value().snapshot->instances()) if (view.entity_id == commanded_ship)
                    for (const auto& ability : view.abilities) if (ability.kind == tactical::AbilityKind::turbo)
                        expect(ability.active == (submitted_tick == 2), "one seat activates and the other deactivates the shared ability");
            }
        }
        const auto produced = result.value().snapshot->events();
        session.events.insert(session.events.end(), produced.begin(), produced.end());
        session.hashes.push_back(result.value().state_sha256);
        return eawr::core::Result<std::string>::success(result.value().state_sha256);
    }
    static std::uint64_t completed_tick(const Session& session) { return session.world.completed_tick(); }
    static std::string state_hash(const Session& session) { return session.world.state_sha256(); }
    static std::vector<std::uint8_t> encoding(const Session& session) { return session.world.canonical_state_bytes(); }
    static eawr::core::Result<std::vector<std::uint8_t>> input_bytes(const Session& session, std::span<const Command> batch) {
        std::vector<std::uint8_t> bytes;
        const auto append = [&](std::uint64_t value) {
            for (unsigned shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        };
        for (const auto& intent : batch) {
            auto replay = session.world.record_through_next_tick();
            replay.commands = {{{intent.key.tick, intent.player, intent.key.sequence}, intent.units, intent.payload}};
            auto encoded = tactical::write_replay(replay);
            if (!encoded) return encoded;
            append(intent.key.seat.value); append(encoded.value().size());
            bytes.insert(bytes.end(), encoded.value().begin(), encoded.value().end());
        }
        return eawr::core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
    }
};

void router_contracts(const Scenario& scenario) {
    auto router = require(tactical::SeatRouter::create(bindings, scenario.setup.players));
    auto commands = script(scenario);
    std::vector<tactical::SeatCommand> batch{commands[1], commands[0]};
    const auto routed = require(router.route(0, batch));
    expect(routed.size() == 2 && routed[0].issuer.seat.value == 1 && routed[1].issuer.seat.value == 2
        && routed[0].command.key.player_id == 1 && routed[1].command.key.player_id == 1
        && routed[0].command.key.sequence == 0 && routed[1].command.key.sequence == 1,
        "reverse arrival routes in canonical seat order with unique faction sequences");
    expect(!router.route(0, batch), "sealed tick cannot be routed twice");
    expect(static_cast<bool>(router.route(1, {})), "empty sealed batch advances");
    expect(!router.route(3, {}), "cannot skip the complete frontier");
    for (const auto mode : {0, 1, 2, 3, 4}) {
        auto fresh = require(tactical::SeatRouter::create(bindings, scenario.setup.players));
        auto invalid = batch;
        if (mode == 0) invalid[0].key.seat.value = 99;
        if (mode == 1) invalid[0].player = 2;
        if (mode == 2) invalid.push_back(invalid.front());
        if (mode == 3) invalid[0].key.tick = 1;
        if (mode == 4) invalid[0].payload = tactical::CreditGrantPayload{units(1)};
        expect(!fresh.route(0, invalid) && fresh.next_tick() == 0, "bad batch leaves router unchanged");
        expect(require(fresh.route(0, batch))[0].command.key.sequence == 0, "retry consumes no rejected sequence");
    }
    for (const auto observer : {false, true}) {
        auto rows = bindings;
        rows[1].observer = observer; rows[1].active = observer;
        auto fresh = require(tactical::SeatRouter::create(rows, scenario.setup.players));
        expect(!fresh.route(0, batch), "inactive and observer seats cannot command");
    }
    auto duplicate = bindings; duplicate[1].seat = duplicate[0].seat;
    expect(!tactical::SeatRouter::create(duplicate, scenario.setup.players), "duplicate roster rejected");
}

void expanded_router_roster() {
    std::vector<tactical::Player> players{{1, 1, 1, tactical::player_flag_commandable},
        {2, 2, 2, tactical::player_flag_commandable}, {3, 3, 3, tactical::player_flag_commandable},
        {4, 3, 4, tactical::player_flag_commandable}};
    std::vector<tactical::SeatBinding> roster;
    for (std::uint32_t id = 1; id <= 9; ++id) roster.push_back({{id}, (id - 1) / 3 + 1, true, false});
    expect(static_cast<bool>(tactical::SeatRouter::create(roster, players)), "nine command seats route across three loaded owners");
    roster.push_back({{10}, 1, true, false});
    expect(!tactical::SeatRouter::create(roster, players), "ten command seats refuse at routing admission");
    roster.pop_back(); roster.back().player = 4;
    expect(static_cast<bool>(tactical::SeatRouter::create(roster, players)), "four owners route even when two share a team");
    players.clear(); roster.clear();
    for (std::uint32_t id = 1; id <= 9; ++id) {
        players.push_back({id, id <= 4 ? 1U : 2U, id, tactical::player_flag_commandable});
        roster.push_back({{id}, id, true, false});
    }
    expect(static_cast<bool>(tactical::SeatRouter::create(roster, players)), "nine distinct allied owners route");
}

void loopback(const Scenario& scenario) {
    using Peer = lockstep::Peer<SharedAdapter>;
    const auto commands = script(scenario);
    std::vector<std::string> reference_hashes;
    std::vector<std::uint8_t> reference_record;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        sim::InlineExecutor inline_executor;
        eawr::platform::ThreadWorkerAdapter pool(workers);
        const sim::PartitionExecutor* executor = workers == 1 ? static_cast<const sim::PartitionExecutor*>(&inline_executor) : &pool;
        for (const auto held : {false, true}) {
            auto split = lockstep::split_by_issuer<SharedAdapter>(commands, 2);
            std::vector<Peer> peers;
            for (std::size_t index = 0; index < 2; ++index) {
                SharedSession session{require(tactical::TacticalSession::create(scenario.setup, scenario.content)),
                    require(tactical::SeatRouter::create(bindings, scenario.setup.players)), {}, {}, {}, {}};
                peers.push_back({std::move(session), executor, std::move(split[index]), {}, {}});
            }
            peers[1].before_step = [](SharedSession& session, std::uint64_t tick) {
                if (tick % 7 == 0) session.world.scramble_storage_for_testing();
                std::reverse(session.pending.begin(), session.pending.end());
            };
            lockstep::Options options;
            options.final_tick = 40 + scenario.build_frames * 2;
            options.compare_every = 1;
            if (held) { options.input_delay = 1; options.hold_every = 7; options.hold_rounds = 4; }
            const auto report = lockstep::run<SharedAdapter>({&peers[0], &peers[1]}, options);
            expect(report.agreed(), report.describe());
            if (!report.agreed()) continue;
            expect(report.remote_commands == commands.size(), "each human seat sends its commands to the other peer");
            if (held) expect(report.total_stalls() > 0, "held transport causes completeness waits");
            expect(peers[0].session.hashes == peers[1].session.hashes, "both peers agree at every tick");
            const auto& provenance = peers[0].session.provenance;
            expect(provenance.size() == commands.size(), "all seat provenance retained");
            expect(provenance[1].command.payload == commands[1].payload, "higher seat's conflicting move is last");
            auto bytes = require(tactical::write_replay(peers[0].session.world.record()));
            if (reference_hashes.empty()) { reference_hashes = peers[0].session.hashes; reference_record = bytes; }
            expect(peers[0].session.hashes == reference_hashes && bytes == reference_record,
                "1/2/4/8 workers and transport delay give identical hashes and recordings");
            const auto replay = require(tactical::parse_replay(bytes));
            auto headless = require(tactical::TacticalSession::from_replay(replay, scenario.content));
            while (headless.completed_tick() < replay.final_tick_count) require(headless.step(*executor));
            expect(headless.state_sha256() == report.final_hash, "recorded replay reproduces final headless state");
            expect(peers[0].session.world.ledgers().front().queues[0].empty(), "both purchases finish in the shared queue");
            expect(peers[0].session.world.ledgers().front().pool.empty(), "both seats deploy from one faction pool");
            for (const auto kind : {tactical::OrderKind::buy, tactical::OrderKind::reinforce}) {
                const auto count = std::count_if(peers[0].session.events.begin(), peers[0].session.events.end(), [&](const auto& event) {
                    return event.kind == tactical::EventKind::order_accepted && event.order == kind && event.player == 1;
                });
                expect(count == 2, "both seats successfully purchase and deploy for the same faction");
            }
            std::cout << "workers=" << workers << " held=" << held << ' ' << report.describe() << '\n';
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        auto scenario = synthetic();
        if (argc > 1 && std::string_view(argv[1]) == "--installed") {
            std::string root;
#ifdef _WIN32
            char* value{}; std::size_t size{};
            if (_dupenv_s(&value, &size, "EAWR_EAW_GAME_ROOT") == 0 && value) root = value;
            std::free(value);
#else
            if (const auto* value = std::getenv("EAWR_EAW_GAME_ROOT")) root = value;
#endif
            if (root.empty()) { std::cout << "SKIPPED: installed game required\n"; return 0; }
            scenario = installed(root);
        }
        expanded_router_roster(); router_contracts(scenario);
        loopback(scenario);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures == 0 ? 0 : 1;
}
