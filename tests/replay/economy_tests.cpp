#include "economy_support.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #530: skirmish purchasing (docs/behaviour/space-purchasing.md). Synthetic two-player sessions
// check income (PU-02 to PU-04), the build queues (PU-10 to PU-18), the reinforcement pool and
// hyperspace arrival (PU-21, PU-30 to PU-39) and the PC cases; one command script must give the
// same hashes at 1, 2, 4 and 8 workers, with scrambled storage and through a written-and-parsed
// replay. A session without economy rules rejects every economy command and hashes as before.
namespace economy_test_support {


int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z) {
    return {units(x), units(y), units(z)};
}
[[nodiscard]] double real(const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(one); }


[[nodiscard]] tactical::TacticalSetup setup() {
    tactical::TacticalSetup result;
    result.seed = 530;
    result.players = {{human, 0, rebel, tactical::player_flag_commandable}, {ai, 1, empire, tactical::player_flag_commandable}};
    result.units = {{human_station, station_type, human, at(-3000, 0), math::identity_quat(), {}},
        {ai_station, station_type, ai, at(3000, 0), math::identity_quat(), {}}};
    return result;
}

[[nodiscard]] tactical::EconomyRules rules(const std::int64_t credits, const std::uint32_t human_cap) {
    tactical::EconomyRules result;
    result.players = {{human, units(credits), human_cap, false, Fixed{}}, {ai, units(credits), 20, true, units(180)}};
    const std::vector<tactical::BuildOption> options{
        {ship_type, tactical::BuildKind::unit, tactical::BuildQueue::units, units(500), 450, 450, 2, true},
        {squadron_type, tactical::BuildKind::unit, tactical::BuildQueue::units, units(550), 510, 510, 1, true},
        {upgrade_type, tactical::BuildKind::upgrade, tactical::BuildQueue::units, Fixed{}, 0, 0, 0, false}};
    for (const auto faction : {rebel, empire}) {
        tactical::StationMenu menu;
        menu.station = station_type;
        menu.faction = faction;
        menu.options = options;
        result.menus.push_back(std::move(menu));
    }
    // 30 per 10 s and a 20 bonus (PU-02, PU-04): 1/10 and 1/15 of a credit per frame.
    result.income = {{station_type, Fixed::from_raw((one + 5) / 10), {{Fixed::from_raw((one + 7) / 15), tactical::always_on}}}};
    result.prevention = {{station_type, units(2000)}};
    // PL-02, LZ-01: the corvette-like ship sinks 90 below the plane (Nebulon-B: -90); the station and
    // the craft have placement boxes that block the arrival search (a craft's is 40 wide).
    const auto box = [](const std::int64_t half) {
        return tactical::PlacementBox{units(-half), units(-half), units(half), units(half)};
    };
    result.footprints = {{ship_type, box(60), units(-90)}, {station_type, box(100), Fixed{}}, {craft_type, box(20), units(40)}};
    result.bounds = std::array<Fixed, 4>{units(-6500), units(-6500), units(6500), units(6500)};
    result.vulnerability = units(-3);
    result.vulnerability_frames = 150;
    result.collision_distance = units(200); // WR-25: synthetic counterpart of the authored constant
    return result;
}

[[nodiscard]] tactical::MotionTable motion() {
    tactical::MotionTable table;
    tactical::CraftProfile craft;
    craft.type_id = craft_type;
    craft.max_speed = Fixed::from_raw(one * 54 / 10);
    craft.min_speed = Fixed::from_raw(one * 18 / 10);
    craft.rate_of_turn = units(6);
    craft.lift = units(6);
    craft.thrust = Fixed::from_raw(one / 5);
    craft.roll_rate = units(6);
    craft.bank_angle = units(70);
    craft.strafe_distance = units(200);
    craft.layer_z = units(40);
    table.squadrons.craft = {craft};
    table.squadrons.squadrons = {{squadron_type, {craft_type, craft_type, craft_type}, {at(0, 0), at(-20, 20), at(-20, -20)},
        units(1000), units(200), units(300), units(20)}};
    return table;
}

[[nodiscard]] tactical::PlayerCommand buy(const std::uint64_t tick, const tactical::PlayerId player, const std::uint64_t sequence,
    const sim::EntityId station, const tactical::TypeId type) {
    return {{tick, player, sequence}, {station}, tactical::BuyPayload{type}};
}

[[nodiscard]] tactical::PlayerCommand cancel(const std::uint64_t tick, const tactical::PlayerId player,
    const std::uint64_t sequence, const std::uint32_t index) {
    return {{tick, player, sequence}, {}, tactical::CancelPayload{0, index}};
}

[[nodiscard]] tactical::PlayerCommand reinforce(const std::uint64_t tick, const tactical::PlayerId player,
    const std::uint64_t sequence, const tactical::TypeId type, const math::Vec3 point) {
    return {{tick, player, sequence}, {}, tactical::ReinforcePayload{type, point}};
}

// Each station sees the whole map, so a unit is visible to the enemy unless hidden (PU-37).
const std::vector<tactical::SensorProfile> sensors{{station_type, units(20000)}};

[[nodiscard]] std::optional<tactical::TacticalSession> session(
    const tactical::EconomyRules& economy, const tactical::MotionTable& table) {
    auto created = tactical::TacticalSession::create(setup(), sensors, {}, table, std::nullopt, {}, {}, {}, economy);
    expect(static_cast<bool>(created), "the economy session is created");
    if (!created) return std::nullopt;
    return std::move(created).value();
}

[[nodiscard]] const tactical::PlayerEconomy* ledger(const tactical::TacticalSession& world, const tactical::PlayerId player) {
    for (const auto& entry : world.ledgers()) {
        if (entry.player == player) return &entry;
    }
    return nullptr;
}

// Steps until `tick` has completed; returns the events of every step.
std::vector<tactical::Event> step_to(tactical::TacticalSession& world, const std::uint64_t tick) {
    const eawr::sim::InlineExecutor executor;
    std::vector<tactical::Event> events;
    while (world.completed_tick() < tick) {
        auto stepped = world.step(executor);
        expect(static_cast<bool>(stepped), "an economy step succeeds");
        if (!stepped) break;
        const auto produced = stepped.value().snapshot->events();
        events.insert(events.end(), produced.begin(), produced.end());
    }
    return events;
}

[[nodiscard]] const tactical::TacticalInstance* instance(const tactical::TacticalSnapshot& snapshot, const sim::EntityId id) {
    for (const auto& entry : snapshot.instances()) {
        if (entry.entity_id == id) return &entry;
    }
    return nullptr;
}
void test_determinism() {
    tactical::TacticalReplay replay;
    replay.setup = setup();
    replay.final_tick_count = 1200;
    replay.commands = {buy(0, human, 0, human_station, squadron_type), buy(0, ai, 0, ai_station, ship_type),
        buy(5, human, 1, human_station, ship_type), cancel(40, human, 2, 1), buy(41, ai, 1, ai_station, squadron_type),
        reinforce(520, human, 3, squadron_type, at(-2500, 1500)), reinforce(520, ai, 2, ship_type, at(2500, -1500)),
        reinforce(1000, ai, 3, squadron_type, at(2500, 1500)),
        {{1001, ai, 4}, {}, tactical::CreditGrantPayload{units(6000)}}};
    const auto run = [&](const tactical::TacticalReplay& input, const eawr::sim::PartitionExecutor& executor, const bool scramble) {
        std::vector<std::string> rows;
        auto content = rules();
        content.vulnerability_frames = 180; // WR-41: replay retains the modifier beyond movement release
        // Exercise deployment together with the partitioned combat world and shared fog rows.
        // Event-only weapons keep the roster alive while proving new ships enter target scans.
        tactical::WeaponProfile weapon;
        weapon.range = units(20000);
        weapon.min_recharge_hundredths = 100;
        weapon.max_recharge_hundredths = 100;
        weapon.cone_width = units(360);
        weapon.cone_height = units(360);
        weapon.opportunity_when_idle = true;
        weapon.opportunity_when_targeting = true;
        tactical::CombatProfile ship;
        ship.type_id = ship_type;
        ship.max_attack_distance = units(20000);
        ship.weapons = {weapon};
        tactical::CombatProfile station;
        station.type_id = station_type;
        station.weapons = {weapon};
        tactical::CombatProfile craft = ship;
        craft.type_id = craft_type;
        const tactical::CombatTable combat{{ship, station, craft}, {}};
        tactical::DurabilityTable health;
        health.profiles = {{ship_type, units(1000), std::nullopt, false, {}},
            {station_type, units(1000), std::nullopt, false, {}},
            {craft_type, units(1000), std::nullopt, false, {}}};
        const tactical::FogRules fog{units(-6500), units(6500), units(100), 130, 130, 16, 21};
        auto created = tactical::TacticalSession::from_replay(input, sensors, health, arrival_lane_motion(), fog,
            combat, {}, {}, content);
        expect(static_cast<bool>(created), "the replay session is created");
        if (!created) return rows;
        auto world = std::move(created).value();
        bool deployed_shooter = false;
        bool deployed_target = false;
        while (world.completed_tick() < input.final_tick_count) {
            if (scramble) world.scramble_storage_for_testing();
            auto stepped = world.step(executor);
            expect(static_cast<bool>(stepped), "a replay step succeeds");
            if (!stepped) break;
            for (const auto& event : stepped.value().snapshot->combat_events()) {
                if (event.kind != tactical::CombatEventKind::weapon_fired) continue;
                deployed_shooter = deployed_shooter || event.shooter > ai_station;
                deployed_target = deployed_target || event.target > ai_station;
                const auto* shooter = instance(*stepped.value().snapshot, event.shooter);
                expect(shooter != nullptr && !shooter->arrival, "PU-39: arriving units hold their fire");
                const auto* target = instance(*stepped.value().snapshot, event.target);
                expect(target != nullptr && (!target->arrival || *target->arrival >= tactical::arrival_visible_frame),
                    "PU-37: hidden arrivals stay out of enemy target scans");
            }
            rows.push_back(stepped.value().state_sha256 + ',' + stepped.value().snapshot->sha256());
        }
        expect(deployed_shooter && deployed_target, "deployed units enter combat as shooters and targets");
        return rows;
    };
    const eawr::sim::InlineExecutor inline_executor;
    const auto reference = run(replay, inline_executor, false);
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(run(replay, executor, false) == reference, std::to_string(workers) + " workers match");
    }
    const eawr::platform::ThreadWorkerAdapter four(4);
    expect(run(replay, four, true) == reference, "scrambled storage matches");
    bool round_trip = false;
    bool replays = false;
    if (const auto written = tactical::write_replay(replay)) {
        const auto parsed = tactical::parse_replay(written.value());
        round_trip = parsed && parsed.value() == replay;
        replays = parsed && run(parsed.value(), inline_executor, false) == reference;
    }
    expect(round_trip, "the buy, cancel and reinforce payloads round-trip");
    expect(replays, "a written-and-parsed replay matches");

    // Without economy rules every economy command is refused and nothing else changes.
    auto blind = tactical::TacticalSession::create(setup(), {}, {}, motion());
    expect(static_cast<bool>(blind), "a session without economy rules is created");
    if (!blind) return;
    auto world = std::move(blind).value();
    expect(world.ledgers().empty() && world.snapshot()->economy().empty(), "no economy, no ledgers");
    expect(static_cast<bool>(world.submit(buy(0, human, 0, human_station, ship_type))), "a buy without an economy");
    const auto events = step_to(world, 1);
    expect(!events.empty() && events.back().reason == tactical::RejectReason::no_economy, "refused: no economy");
}

void test_rotated_reinforcement() {
    auto content = rules();
    for (auto& menu : content.menus) for (auto& option : menu.options)
        option.build_frames = option.ai_build_frames = 1;
    const auto table = arrival_lane_motion();
    auto command = reinforce(3, human, 1, ship_type, at(-2850, 0));
    std::get<tactical::ReinforcePayload>(command.payload).facing_yaw = units(180);
    const tactical::TacticalReplay replay{setup(), 160, {buy(0, human, 0, human_station, ship_type), command}};
    const auto written = tactical::write_replay(replay);
    expect(static_cast<bool>(written), "WR-X01: explicit-facing replay writes");
    if (!written) return;
    const auto parsed = tactical::parse_replay(written.value());
    expect(parsed && parsed.value() == replay, "WR-X01: explicit yaw and position round-trip through opcode 26");
    if (!parsed) return;
    for (const auto yaw : {units(0), units(90), Fixed::from_raw(359 * one + one / 2)}) {
        auto varied = replay;
        auto& payload = std::get<tactical::ReinforcePayload>(varied.commands.back().payload);
        payload.facing_yaw = yaw;
        payload.pool_token = 7;
        const auto bytes = tactical::write_replay(varied);
        expect(static_cast<bool>(bytes), "WR-X01: rotated reserved command writes");
        if (bytes) {
            const auto restored = tactical::parse_replay(bytes.value());
            expect(restored && restored.value() == varied, "WR-X01: zero/fractional yaw and reserved pool token survive serialization");
        }
    }
    for (const auto yaw : {units(-1), units(360)}) {
        auto invalid = replay;
        std::get<tactical::ReinforcePayload>(invalid.commands.back().payload).facing_yaw = yaw;
        expect(!tactical::write_replay(invalid), "WR-X01: noncanonical facing is rejected at the command boundary");
    }
    std::vector<std::string> expected;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::from_replay(parsed.value(), sensors, {}, table, std::nullopt,
            {}, {}, {}, content);
        expect(static_cast<bool>(created), "WR-X01: recorded facing creates a session");
        if (!created) return;
        auto world = std::move(created).value();
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> trace;
        while (world.completed_tick() < replay.final_tick_count) {
            if (workers != 1) world.scramble_storage_for_testing();
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "WR-X01: rotated reinforcement replay steps");
            if (!step) return;
            trace.push_back(step.value().state_sha256 + ',' + step.value().snapshot->sha256());
            if (world.completed_tick() == 4) {
                expect(ledger(world, human)->pool.empty() && world.arrivals().size() == 1,
                    "WR-X01: authoritative lane query accepts reversed facing past the same blocker");
                if (!world.arrivals().empty()) {
                    const auto& arrival = world.arrivals().begin()->second;
                    expect(arrival.direction.x.raw() < -one + 10 && std::abs(arrival.direction.y.raw()) < 10,
                        "WR-X01: arrival lane flies along the command's facing");
                }
            }
        }
        if (expected.empty()) expected = trace;
        expect(trace == expected, "WR-X01: recorded facing hashes agree for 1/2/4/8 workers and scrambled storage");
    }
}

void test_reserved_reinforcement() {
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto content = rules();
        for (auto& menu : content.menus) for (auto& option : menu.options)
            option.build_frames = option.ai_build_frames = 1;
        auto world = session(content);
        if (!world) return;
        expect(static_cast<bool>(world->submit(buy(0, human, 0, human_station, ship_type))), "SAE-11: first purchase submitted");
        expect(static_cast<bool>(world->submit(buy(0, human, 1, human_station, ship_type))), "SAE-11: second same-type purchase submitted");
        step_to(*world, 3);
        const auto tokens = ledger(*world, human)->pool_tokens;
        expect(tokens.size() == 2 && tokens[0] != tokens[1], "SAE-11: identical types get distinct pool identities");
        if (tokens.size() != 2) return;
        const math::Vec3 point = at(-2000, 1000);
        tactical::PlayerCommand reserved{{3, human, 2}, {}, tactical::ReinforcePayload{ship_type, point, tokens[1]}};
        tactical::PlayerCommand duplicate = reserved;
        duplicate.key.sequence = 3;
        tactical::TacticalReplay replay;
        replay.setup = setup();
        replay.final_tick_count = 4;
        replay.commands = {reserved, duplicate};
        const auto written = tactical::write_replay(replay);
        expect(static_cast<bool>(written), "SAE-11: purchase-token replay writes");
        if (written) {
            const auto parsed = tactical::parse_replay(written.value());
            expect(parsed && parsed.value() == replay, "SAE-11: purchase-token command round-trips through opcode 19");
        }
        expect(static_cast<bool>(world->submit(reserved)) && static_cast<bool>(world->submit(duplicate)),
            "SAE-11: duplicate reserved requests enter ordinary admission");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto stepped = world->step(executor);
        expect(static_cast<bool>(stepped), "SAE-11: authoritative reserved admission succeeds");
        if (!stepped) return;
        const auto events = stepped.value().snapshot->events();
        expect(events.size() == 2 && events[0].kind == tactical::EventKind::order_accepted
            && events[1].reason == tactical::RejectReason::not_in_pool,
            "SAE-11: only one duplicate is admitted; missing identity never falls back to type");
        expect(ledger(*world, human)->pool_tokens == std::vector<std::uint64_t>{tokens[0]},
            "SAE-11: non-front reservation consumed and the other identical purchase survives");
        const auto admitted = events.empty() ? eawr::sim::invalid_entity_id : events[0].unit;
        const auto purchase_token = [&]() {
            for (const auto& unit : world->units()) if (unit.entity_id == admitted) return unit.purchase_token;
            return std::uint64_t{};
        };
        expect(purchase_token() == tokens[1], "SAE-11: primary arrival carries its admitted reservation through ECS commit");
        std::vector<std::string> trace{stepped.value().state_sha256, stepped.value().snapshot->sha256()};
        while (world->completed_tick() < 160) {
            const auto flight = world->step(executor);
            expect(static_cast<bool>(flight), "SAE-11: admitted unit advances through its flyout");
            if (!flight) break;
        }
        expect(purchase_token() == tokens[1], "SAE-11: purchase identity survives after flyout removal and later ECS updates");
        trace.push_back(world->state_sha256());
        if (workers == 1) baseline = trace;
        else expect(trace == baseline, "SAE-11: admission and canonical state match at 1/2/4/8 workers");
    }
}

// validate_economy and the command shapes.
void test_validation() {
    const auto players = setup().players;
    expect(static_cast<bool>(tactical::validate_economy(rules(), players)), "the synthetic rules are valid");
    auto stranger = rules();
    stranger.players[1].player = 7;
    expect(!tactical::validate_economy(stranger, players), "an economy player must be declared");
    auto free = rules();
    free.menus[0].options[0].price = Fixed{};
    expect(!tactical::validate_economy(free, players), "an available option needs a price");
    auto world = session();
    if (!world) return;
    expect(!world->submit({{0, human, 0}, {}, tactical::BuyPayload{ship_type}}), "a buy lists its station");
    expect(!world->submit({{0, human, 1}, {human_station}, tactical::CancelPayload{0, 0}}), "a cancel lists no unit");
    expect(!world->submit({{0, human, 2}, {}, tactical::CancelPayload{2, 0}}), "a cancel names a queue");
    auto reserved_start = setup();
    reserved_start.units.front().purchase_token = 1;
    expect(!tactical::validate_setup(reserved_start), "SAE-11: initial setup cannot inject a runtime purchase identity");
}

} // namespace

using namespace economy_test_support;

int main() {
    expect(tactical::reinforcement_prevention_blocks(at(0, 0), units(5), at(2, 3, 1000)),
        "WR-22: prevention is planar and rejects the circle interior");
    expect(!tactical::reinforcement_prevention_blocks(at(0, 0), units(5), at(3, 4)),
        "WR-22: equality at a prevention radius is allowed");
    expect(!tactical::reinforcement_prevention_blocks(at(0, 0), Fixed{}, at(0, 0)),
        "WR-22: a nonpositive radius never blocks");
    const std::optional<std::array<Fixed, 4>> bounds{{units(-10), units(-20), units(10), units(20)}};
    expect(tactical::reinforcement_inside_bounds(bounds, at(-10, 20)), "WR-23: playable bounds include equality");
    expect(!tactical::reinforcement_inside_bounds(bounds, at(11, 0)), "WR-23: outside playable bounds is refused");
    test_roster_gate();
    test_arrival_table();
    test_income();
    test_credit_adjustments();
    test_team_production();
    test_buy();
    test_credit_grant();
    test_refusals();
    test_cancel();
    test_cancel_entry();
    test_station_lost();
    test_reinforce_ship();
    test_reinforced_carrier();
    test_arrival_hits();
    test_arrival_services();
    test_pending_victory_reinforcement();
    test_arrival_lane_sweep();
    test_arrival_tracking_frames();
    test_reinforce_refusals();
    test_arrival_placement();
    test_determinism();
    test_validation();
    test_reserved_reinforcement();
    test_rotated_reinforcement();
    if (failures != 0) {
        std::cerr << failures << " economy check(s) failed\n";
        return 1;
    }
    std::cout << "economy contracts passed\n";
    return 0;
}
