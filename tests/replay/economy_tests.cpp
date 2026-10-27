// #530: skirmish purchasing (docs/behaviour/space-purchasing.md). Synthetic two-player sessions
// check income (PU-02 to PU-04), the build queues (PU-10 to PU-18), the reinforcement pool and
// hyperspace arrival (PU-21, PU-30 to PU-39) and the PC cases; one command script must give the
// same hashes at 1, 2, 4 and 8 workers, with scrambled storage and through a written-and-parsed
// replay. A session without economy rules rejects every economy command and hashes as before.

#include "../support/check.hpp"
#include "economy_support.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace economy_test_support {


eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
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

void test_campaign_reserve_publication() {
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto start = setup();
        start.first_free_entity_id = 601; // shared allocator follows every initial reserve
        start.campaign.emplace();
        start.campaign->reserves = {{500, ship_type, human, true, squadron_type, {}},
            {501, ship_type, human, false, ship_type, {}}, {600, ship_type, ai, false, 0, {}}};
        auto content = rules(); content.campaign = true;
        for (auto& player : content.players) player.credit_cap = units(6000);
        start.campaign->players = content.players;
        content.campaign_population = {{ship_type, 2}, {squadron_type, 1}};
        auto created = tactical::TacticalSession::create(start, sensors, {}, motion(), std::nullopt, {}, {}, {}, content);
        expect(static_cast<bool>(created), "WGM-48: campaign publication fixture is valid");
        if (!created) return;
        auto world = std::move(created).value();
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> trace;
        for (unsigned step = 0; step < 3; ++step) {
            const auto snapshot = world.snapshot();
            const auto rows = snapshot->economy();
            const auto local = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.player == human; });
            const auto enemy = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.player == ai; });
            expect(local != rows.end() && local->reserves == std::vector<tactical::ReserveView>{
                {500, ship_type, 1, true}, {501, ship_type, 2, false}},
                "WGT-19/28/WR-09: equal current types retain independent original populations and restrictions");
            expect(enemy != rows.end() && enemy->reserves == std::vector<tactical::ReserveView>{{600, ship_type, 2, false}},
                "reserve rows stay partitioned by published economy owner");
            auto advanced = world.step(executor);
            expect(static_cast<bool>(advanced), "reserve publication advances through ordinary tactical step");
            if (!advanced) return;
            trace.push_back(advanced.value().state_hash.get());
        }
        if (baseline.empty()) baseline = trace;
        expect(trace == baseline, "reserve publication retains 1/2/4/8 tactical hash equality");
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

void test_controller_respawn_attribution() {
    for (const auto humans : {2U, 3U, 4U}) for (const bool passive_departure : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto start = setup(); auto content = rules();
            content.campaign = true;
            for (auto& player : content.players) player.credit_cap = units(6000);
            start.campaign.emplace(); start.campaign->players = content.players;
            start.first_free_entity_id = 4;
            content.pads.respawn = {{ship_type, 5}};
            content.pads.neutral = ai;
            tactical::CaptureProfile point;
            point.type = station_type; point.radius = units(100); point.transition_seconds = units(1);
            point.affiliation = {rebel, empire}; point.ownership_sticks = true; point.build_pad = true;
            content.pads.capture = {point};
            content.pads.construction = {{30, 31, units(100), 1, 1, true}};
            content.menus.front().options.push_back({30, tactical::BuildKind::structure, tactical::BuildQueue::units,
                units(100), 1, 1, 0, true});
            start.controllers.emplace();
            start.controllers->factions = {{human, tactical::FactionController::humans}, {ai, tactical::FactionController::ai}};
            for (std::uint32_t seat = 1; seat <= humans; ++seat)
                start.controllers->seats.push_back({{seat}, human, sim::SeatKind::human, true});
            start.controllers->seats.push_back({{humans + 1}, ai, sim::SeatKind::ai, true});
            for (auto& unit : start.units) unit.control = tactical::UnitControl::shared_structure;
            start.units.push_back({3, ship_type, human, at(-2000, 1000)});
            start.units.back().control = tactical::UnitControl::mobile; start.units.back().controller_seat = {humans};
            tactical::DurabilityTable health;
            tactical::DurabilityProfile hull; hull.type_id = ship_type; hull.max_hull = units(100);
            health.profiles.push_back(hull);
            for (const auto type : {30U, 31U}) {
                hull.type_id = type; health.profiles.push_back(hull);
            }
            auto made = tactical::TacticalSession::create(start, sensors, health, motion(), std::nullopt, {}, {}, {}, content);
            expect(static_cast<bool>(made), "P3: respawn attribution fixture creates");
            if (!made) continue;
            auto world = std::move(made).value();
            const auto structure = world.stage_spawn({0, station_type, human, at(-4000, 0)});
            expect(static_cast<bool>(structure), "P3: runtime structure enters through the trusted creation seam");
            if (structure) {
                const auto* published = instance(*world.snapshot(), structure.value());
                expect(published && published->control == tactical::UnitControl::shared_structure
                    && published->controller_seat.value == 0, "P3: newly created structures always enter shared");
            }
            tactical::PlayerCommand damage{{0, human, 0}, {3}, tactical::DamagePayload{units(1000)}};
            damage.origin = tactical::CommandOrigin::engine;
            expect(static_cast<bool>(world.submit(damage)), "P3: automatic replacement trigger submits");
            tactical::PlayerCommand build{{0, human, 1}, {human_station}, tactical::PadBuildPayload{30}};
            build.origin = tactical::CommandOrigin::seat; build.issuing_seat = {humans};
            expect(static_cast<bool>(world.submit(build)), "P3: authenticated shared-pad construction submits");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::optional<tactical::TacticalSession> restored;
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 40; ++frame) {
                if (passive_departure && frame == 1) {
                    auto roster = *world.controller_policy();
                    for (std::uint32_t seat = 1; seat <= humans; ++seat) {
                        tactical::BattleControlPayload change; change.departed = {seat};
                        for (auto& row : roster.seats) if (row.player == human) {
                            if (row.seat.value == seat) row.active = false;
                            change.policy.seats.push_back(row);
                        }
                        change.policy.factions.push_back({human, seat == humans
                            ? tactical::FactionController::passive : tactical::FactionController::humans});
                        tactical::PlayerCommand departure{{frame, human, seat}, {}, change};
                        departure.origin = tactical::CommandOrigin::engine;
                        expect(static_cast<bool>(world.submit(departure)), "P5: departure submits while respawn is scheduled");
                        if (restored) expect(static_cast<bool>(restored->submit(departure)), "P5: restored pending respawn receives departure");
                    }
                }
                if (workers != 1) world.scramble_storage_for_testing();
                const auto advanced = world.step(executor);
                expect(static_cast<bool>(advanced), "P3: automatic replacement advances");
                if (!advanced) break;
                hashes.push_back(advanced.value().state_sha256 + advanced.value().snapshot->sha256());
                if (restored) {
                    const auto resumed = restored->step(executor);
                    expect(resumed && resumed.value().state_sha256 == advanced.value().state_sha256
                        && resumed.value().snapshot->canonical_bytes() == advanced.value().snapshot->canonical_bytes(),
                        "P3: pending last-controller respawn checkpoint continues exactly");
                }
                if (frame == 0 || (passive_departure && frame == 1)) {
                    const auto saved = world.save_checkpoint_state();
                    auto fresh = tactical::TacticalSession::from_replay(world.record(), sensors, health, motion(), std::nullopt, {}, {}, {}, content);
                    expect(saved && fresh, "P3: respawn checkpoint factory creates");
                    if (saved && fresh) {
                        const auto loaded = fresh.value().load_checkpoint_state(saved.value());
                        expect(static_cast<bool>(loaded), "P3: pending respawn retains last controller through load");
                        if (!loaded) { std::cerr << loaded.error().message << '\n'; break; }
                        restored.emplace(std::move(fresh).value());
                    }
                }
            }
            bool appeared{}, constructed{};
            for (const auto& unit : world.units()) {
                if (unit.type_id == ship_type) {
                    appeared = true;
                    expect(unit.entity_id != 3 && unit.control == tactical::UnitControl::mobile
                        && unit.controller_seat.value == humans, "P3: automatic replacement keeps its last eligible controller");
                }
                if (unit.type_id == 31) {
                    constructed = true;
                    expect(unit.control == tactical::UnitControl::shared_structure && unit.controller_seat.value == 0,
                        "P3: completed construction stays shared despite its human initiator");
                }
            }
            expect(appeared, "P3/P5: replacement actually appears after the authored delay, including passive departure");
            if (passive_departure) expect(world.controller_policy()->factions.front().mode == tactical::FactionController::passive,
                "P5: scheduled replacement retains inactive caller identity without reviving human or AI control");
            expect(constructed, "P3: requested construction actually completes");
            if (reference.empty()) reference = hashes;
            expect(hashes == reference, "P3: respawn attribution equals on 1/2/4/8 workers");
        }
    }
}

void test_controller_output_attribution() {
    for (const auto humans : {2U, 3U, 4U}) for (const bool reserves : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto start = setup();
            auto content = rules(); content.campaign = true;
            constexpr tactical::TypeId mobile_producer_type = 41;
            auto movement = motion();
            movement.rules = {units(15), units(300)};
            movement.profiles = {{mobile_producer_type, units(3), units(1), units(1), units(1), units(1), units(1), units(5)}};
            content.heroes = {{ship_type, ship_type, {{101, true, false}}}};
            for (auto& player : content.players) player.credit_cap = units(6000);
            for (auto& menu : content.menus) for (auto& option : menu.options)
                option.build_frames = option.ai_build_frames = 5;
            auto mobile_menu = content.menus.front(); mobile_menu.station = mobile_producer_type;
            content.menus.push_back(mobile_menu);
            start.campaign.emplace(); start.campaign->players = content.players;
            if (reserves) {
                start.campaign->reserves = {{500, squadron_type, human, false, 0, {}},
                    {501, ship_type, human, false, 0, {{.entity_id = 499, .type_id = 101, .parent = 501, .named_hero = true, .generic_hero = false, .members = {}}}}};
                start.campaign->reserves.back().contained.front().members.push_back({.entity_id = 498, .type_id = 102, .parent = 499, .named_hero = false, .generic_hero = true, .members = {}});
                content.campaign_population = {{ship_type, 2}, {squadron_type, 1}};
            }
            start.first_free_entity_id = 502;
            start.controllers.emplace();
            start.controllers->factions = {{human, tactical::FactionController::humans}, {ai, tactical::FactionController::ai}};
            for (std::uint32_t seat = 1; seat <= humans; ++seat)
                start.controllers->seats.push_back({{seat}, human, sim::SeatKind::human, true});
            start.controllers->seats.push_back({{humans + 1}, ai, sim::SeatKind::ai, true});
            for (auto& unit : start.units) unit.control = tactical::UnitControl::shared_structure;
            start.units.push_back({3, mobile_producer_type, human, at(-4000, 0)});
            start.units.back().control = tactical::UnitControl::mobile;
            start.units.back().controller_seat = {humans - 1};
            auto made = tactical::TacticalSession::create(start, sensors, {}, movement, std::nullopt, {}, {}, {}, content);
            expect(static_cast<bool>(made), "P3: reserve and queue attribution fixture creates for 2/3/4 humans");
            if (!made) continue;
            auto world = std::move(made).value();
            const auto issue = [&](tactical::PlayerCommand command, const std::uint32_t seat) {
                command.origin = seat == 0 ? tactical::CommandOrigin::engine : tactical::CommandOrigin::seat;
                command.issuing_seat = {seat};
                expect(static_cast<bool>(world.submit(command)), "P3: attributed output request submits");
            };
            if (reserves) {
                issue({{2, human, 2}, {}, tactical::ReinforcePayload{squadron_type, at(-2000, 1000), 500}}, humans);
                issue({{2, human, 3}, {}, tactical::ReinforcePayload{squadron_type, at(-2000, 1000), 500}}, 1);
                issue({{3, human, 4}, {}, tactical::ReinforcePayload{ship_type, at(-2000, -1000), 501}}, 1);
            } else {
                issue(buy(0, human, 0, human_station, ship_type), humans);
                issue(buy(0, human, 1, human_station, squadron_type), 1);
                issue(buy(0, human, 2, 3, ship_type), 0);
                // GC deploys only its sealed initial reserves. Queue output retains its
                // initiator in the shared pool without adding a purchase-to-deploy route.
            }
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::optional<tactical::TacticalSession> restored;
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 170; ++frame) {
                if (reserves && frame == 3) {
                    // Gift one already-arriving call, then the hero created earlier in this
                    // same command batch. Neither request changes persistent faction ownership.
                    for (auto command : std::vector<tactical::PlayerCommand>{
                        {{3, human, 5}, {500}, tactical::GiftPayload{{1}}, {humans}, tactical::CommandOrigin::seat},
                        {{3, human, 6}, {501}, tactical::GiftPayload{{humans}}, {1}, tactical::CommandOrigin::seat}}) {
                        expect(static_cast<bool>(world.submit(command)), "P4: arriving squadron and same-batch hero gifts submit");
                        if (restored) expect(static_cast<bool>(restored->submit(command)), "P4: checkpoint receives identical arriving gifts");
                    }
                }
                if (workers != 1) world.scramble_storage_for_testing();
                const auto advanced = world.step(executor);
                expect(static_cast<bool>(advanced), "P3: queued and arriving output advances");
                if (!advanced) { std::cerr << advanced.error().message << '\n'; break; }
                hashes.push_back(advanced.value().state_sha256 + advanced.value().snapshot->sha256());
                if (restored) {
                    const auto resumed = restored->step(executor);
                    expect(resumed && resumed.value().state_sha256 == advanced.value().state_sha256
                        && resumed.value().snapshot->canonical_bytes() == advanced.value().snapshot->canonical_bytes(),
                        "P3: checkpoint continuation equals live through queued, pooled and arriving output");
                    if (!resumed) break;
                }
                if (frame == 0 && !reserves) {
                    const auto* account = ledger(world, human);
                    expect(account && account->queues[0].size() == 3
                        && account->queues[0][0].controller_seat.value == humans
                        && account->queues[0][1].controller_seat.value == 1
                        && account->queues[0][2].controller_seat.value == humans - 1,
                        "P3: shared queue retains human callers and an automatic mobile producer's current controller");
                }
                if (frame == 2 && reserves) {
                    const auto events = advanced.value().snapshot->events();
                    expect(std::count_if(events.begin(), events.end(), [](const auto& event) {
                        return event.kind == tactical::EventKind::order_accepted;
                    }) == 1 && std::count_if(events.begin(), events.end(), [](const auto& event) {
                        return event.reason == tactical::RejectReason::not_in_pool;
                    }) == 1, "P3: canonical first caller consumes one shared persistent reserve");
                }
                if (frame == 0 || frame == 10 || frame == 14) {
                    const auto saved = world.save_checkpoint_state();
                    auto log = world.record();
                    const auto pending = world.pending_commands();
                    log.commands.insert(log.commands.end(), pending.begin(), pending.end());
                    log.final_tick_count = 170;
                    auto fresh = tactical::TacticalSession::from_replay(log, sensors, {}, movement, std::nullopt, {}, {}, {}, content);
                    expect(saved && fresh, "P3: pending-output checkpoint and sealed factory create");
                    if (saved && fresh) {
                        const auto loaded = fresh.value().load_checkpoint_state(saved.value());
                        expect(static_cast<bool>(loaded), "P3: pending-output checkpoint loads");
                        if (!loaded) { std::cerr << loaded.error().message << '\n'; break; }
                        const auto recaptured = fresh.value().save_checkpoint_state();
                        expect(recaptured && recaptured.value() == saved.value()
                            && fresh.value().state_sha256() == world.state_sha256(),
                            "P3: queue callers, pool bindings and accepted arrivals round-trip exactly");
                        restored.emplace(std::move(fresh).value());
                    }
                }
            }
            const auto live = world.units();
            const auto root = [&](const sim::EntityId id) -> const tactical::UnitState* {
                const auto found = std::find_if(live.begin(), live.end(), [&](const auto& unit) { return unit.entity_id == id; });
                return found == live.end() ? nullptr : &*found;
            };
            if (reserves) {
                expect(root(500) && root(500)->controller_seat.value == 1 && root(500)->owner == human,
                    "P4: gifted arriving reserve keeps its persistent faction and receiving controller");
                expect(root(501) && root(501)->controller_seat.value == humans && root(501)->owner == human
                    && root(501)->contained == start.campaign->reserves.back().contained,
                    "P4: same-batch called hero is giftable with stable identity and nested passengers");
            }
            for (const auto& group : world.squadrons()) {
                const auto* container = root(group.container);
                expect(container && container->control == tactical::UnitControl::mobile,
                    "P3: reinforced squadron has an independently controlled root");
                if (!container) continue;
                for (const auto id : group.members) expect(root(id) && root(id)->controller_seat == container->controller_seat,
                    "P3: every reinforced squadron member inherits its root's caller");
            }
            const auto* account = ledger(world, human);
            if (reserves) expect(account->pool.empty() && account->pool_controllers.empty(),
                "P3: consumed reserve identities retire their pending controller bindings");
            else expect(account->queues[0].empty() && account->pool.size() == 3
                && account->pool_controllers == std::map<std::uint64_t, sim::SeatId>{{1, {humans}}, {2, {1}}, {3, {humans - 1}}},
                "P3: pooled output retains both human initiators and its automatic mobile producer controller");
            auto replayed = tactical::TacticalSession::from_replay(world.record(), sensors, {}, movement, std::nullopt, {}, {}, {}, content);
            expect(static_cast<bool>(replayed), "P3: output child replay creates");
            if (replayed) {
                while (replayed.value().completed_tick() < world.completed_tick()) {
                    if (!replayed.value().step(executor)) { expect(false, "P3: output replay advances"); break; }
                }
                expect(replayed.value().state_sha256() == world.state_sha256()
                    && replayed.value().snapshot()->canonical_bytes() == world.snapshot()->canonical_bytes(),
                    "P3: recorded child output equals live state and snapshot");
            }
            if (reference.empty()) reference = hashes;
            expect(hashes == reference, "P3: output attribution equals on 1/2/4/8 workers with scrambled storage");
        }
    }
}

void test_controller_shared_economy() {
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto start = setup();
        start.first_free_entity_id = 5;
        start.campaign.emplace();
        auto content = rules(4000, 20); content.campaign = true;
        for (auto& player : content.players) player.credit_cap = units(6000);
        start.campaign->players = content.players;
        start.controllers = tactical::BattleControllerPolicy{
            {{{1}, human, sim::SeatKind::human, true}, {{2}, human, sim::SeatKind::human, true},
                {{3}, ai, sim::SeatKind::ai, true}},
            {{human, tactical::FactionController::humans}, {ai, tactical::FactionController::ai}}};
        for (auto& station : start.units) station.control = tactical::UnitControl::shared_structure;
        // A mod can put a production menu on a mobile hull: this remains private.
        start.units.push_back({3, station_type, human, at(-2000, 0)});
        start.units.back().control = tactical::UnitControl::mobile; start.units.back().controller_seat = {1};
        tactical::DurabilityTable health;
        tactical::DurabilityProfile station; station.type_id = station_type; station.max_hull = units(100);
        tactical::HardpointProfile hardpoint; hardpoint.destroyable = true; hardpoint.max_health = units(100);
        station.hardpoints.push_back(hardpoint); health.profiles.push_back(station);
        auto made = tactical::TacticalSession::create(start, sensors, health, motion(), std::nullopt, {}, {}, {}, content);
        expect(static_cast<bool>(made), "co-op economy fixture creates");
        if (!made) continue;
        auto world = std::move(made).value();
        world.scramble_storage_for_testing();
        std::uint64_t sequence{};
        const auto issue = [&](tactical::PlayerCommand command, const std::uint32_t seat) {
            command.key.sequence = ++sequence; command.origin = tactical::CommandOrigin::seat; command.issuing_seat = {seat};
            expect(static_cast<bool>(world.submit(command)), "co-op economy command reaches authoritative execution");
        };
        issue(buy(0, human, 0, human_station, ship_type), 1);
        issue(buy(0, human, 0, human_station, ship_type), 2);
        issue(buy(0, human, 0, 3, ship_type), 2);
        issue(buy(0, human, 0, 3, ship_type), 1);
        issue(cancel(0, human, 0, 0), 2);
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto tick = world.step(executor);
        expect(static_cast<bool>(tick), "co-op shared queue advances");
        if (!tick) continue;
        const auto events = tick.value().snapshot->events();
        expect(std::count_if(events.begin(), events.end(), [](const auto& event) {
            return event.kind == tactical::EventKind::order_accepted;
        }) == 4, "both seats buy and cancel in the shared queue while the mobile producer accepts its controller");
        expect(std::count_if(events.begin(), events.end(), [](const auto& event) {
            return event.reason == tactical::RejectReason::unit_not_controlled;
        }) == 1, "forged same-faction mobile producer order is rejected before spending credits");
        expect(ledger(world, human)->queues[0].size() == 2, "one faction queue remains shared between seats");
        tactical::PlayerCommand damage{{1, human, ++sequence}, {ai_station}, tactical::DamagePayload{units(50), 0}};
        damage.origin = tactical::CommandOrigin::engine;
        expect(static_cast<bool>(world.submit(damage)), "trusted damage prepares a damaged opposing station");
        for (const auto seat : {1U, 2U}) issue({{1, human, 0}, {ai_station}, tactical::RepairHardpointPayload{0}}, seat);
        const auto repaired = world.step(executor);
        expect(repaired && std::count_if(repaired.value().snapshot->events().begin(), repaired.value().snapshot->events().end(), [](const auto& event) {
            return event.kind == tactical::EventKind::order_accepted && event.order == tactical::OrderKind::repair_hardpoint;
        }) == 2, "repair keeps its payer exception for both seats even on another faction's shared station");
        const std::vector<std::string> hashes{world.state_sha256(), world.snapshot()->sha256()};
        if (reference.empty()) reference = hashes;
        expect(hashes == reference, "shared economy controllers keep 1/2/4/8 worker digests equal");
    }
}

} // namespace

using namespace economy_test_support;

int main(const int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--vertical-arrival-blocker") {
        test_vertical_arrival_blocker();
        return failures == 0 ? 0 : 1;
    }
    expect(tactical::reinforcement_prevention_blocks(at(0, 0), units(5), at(2, 3, 1000)),
        "WR-22: prevention is planar and rejects the circle interior");
    expect(!tactical::reinforcement_prevention_blocks(at(0, 0), units(5), at(3, 4)),
        "WR-22: equality at a prevention radius is allowed");
    expect(!tactical::reinforcement_prevention_blocks(at(0, 0), Fixed{}, at(0, 0)),
        "WR-22: a nonpositive radius never blocks");
    const std::optional<std::array<Fixed, 4>> bounds{{units(-10), units(-20), units(10), units(20)}};
    expect(tactical::reinforcement_inside_bounds(bounds, at(-10, 20)), "WR-23: playable bounds include equality");
    expect(!tactical::reinforcement_inside_bounds(bounds, at(11, 0)), "WR-23: outside playable bounds is refused");
    test_controller_respawn_attribution();
    test_controller_output_attribution();
    test_controller_shared_economy();
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
    test_arrival_widens_drop_area();
    test_pending_victory_reinforcement();
    test_arrival_lane_sweep();
    test_arrival_tracking_frames();
    test_reinforce_refusals();
    test_arrival_placement();
    test_vertical_arrival_blocker();
    test_determinism();
    test_validation();
    test_reserved_reinforcement();
    test_campaign_reserve_publication();
    test_rotated_reinforcement();
    if (failures != 0) {
        std::cerr << failures << " economy check(s) failed\n";
        return 1;
    }
    std::cout << "economy contracts passed\n";
    return 0;
}
