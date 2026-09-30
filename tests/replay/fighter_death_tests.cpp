#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// #447 spin-away deaths (docs/behaviour/space-fighter-deaths.md, cases SC-01 to SC-07): the keyed
// chance (SP-02), the spin's timing and path (SP-04 to SP-08) and a session's kill, spin and
// explosion timeline with its worker-count hash equality.
namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr std::uint64_t seed = 4471;
constexpr tactical::PlayerId empire = 1;
constexpr tactical::PlayerId rebel = 2;
constexpr tactical::TypeId craft_type = 200;
constexpr tactical::TypeId squadron_type = 300;

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] Fixed tenths(const std::int64_t value) { return Fixed::from_raw(value * one / 10); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}

[[nodiscard]] tactical::SpinAwayProfile spin(const Fixed chance) { return {chance, units(2)}; }

// SC-01 (SP-02): the draw is keyed, and its share of spins follows the chance.
void test_chance() {
    int spun = 0;
    int always = 0;
    int never = 0;
    constexpr int kills = 4000;
    for (int index = 0; index < kills; ++index) {
        const auto frame = static_cast<std::uint64_t>(100 + index / 7);
        const auto unit = static_cast<eawr::sim::EntityId>(10 + index);
        const bool first = tactical::spins_away(spin(tenths(2)), seed, frame, unit);
        expect(first == tactical::spins_away(spin(tenths(2)), seed, frame, unit), "SC-01: the draw is keyed");
        spun += first ? 1 : 0;
        always += tactical::spins_away(spin(units(1)), seed, frame, unit) ? 1 : 0;
        never += tactical::spins_away(spin(Fixed{}), seed, frame, unit) ? 1 : 0;
    }
    expect(spun > kills * 17 / 100 && spun < kills * 23 / 100, "SC-01: chance 0.2 spins about one kill in five");
    expect(always == kills, "SC-01: chance 1 always spins");
    expect(never == 0, "SC-01: chance 0 does not spin");
}

[[nodiscard]] tactical::CraftState flying(const Fixed roll) {
    tactical::CraftState state;
    state.roll = roll;
    state.velocity = {units(5), Fixed{}, Fixed{}};
    return state;
}

[[nodiscard]] tactical::SpinAwayProfile spin_profile() { return spin(tenths(2)); }

// Services a spin from `frame` until it ends; the number of services, or nullopt past `limit`.
[[nodiscard]] std::optional<int> run(tactical::DeathSpin& spin, const std::uint64_t frame, const int limit,
    std::vector<tactical::DeathSpin>* trace = nullptr) {
    for (int service = 1; service <= limit; ++service) {
        const auto stepped = tactical::step_spin(spin, units(6), spin_profile(), seed, frame + static_cast<std::uint64_t>(service));
        if (!stepped) {
            expect(false, "a spin step failed: " + stepped.error().message);
            return std::nullopt;
        }
        if (stepped.value()) return service;
        if (trace != nullptr) trace->push_back(spin);
    }
    return std::nullopt;
}

// SC-02 (SP-04 to SP-08): a craft at 5 units a frame spins for 2 s: it explodes at the 60th
// service, rolls 20 degrees a frame, stays near its straight run and never bows downward.
void test_timing() {
    auto spinning = tactical::start_spin(7, craft_type, empire, at(100, 200, 50), flying(Fixed{}));
    std::vector<tactical::DeathSpin> trace;
    const auto services = run(spinning, 500, 200, &trace);
    expect(services == 60, "SC-02: the spin ends at the 60th service (2 s)");
    expect(trace.size() == 59, "SC-02: it moves 59 times");
    if (trace.size() != 59) return;
    expect(trace[0].path && trace[0].points[0] == at(100, 200, 50) && trace[0].points[3] == at(400, 200, 50),
        "SP-05: the path runs from the kill point along the velocity for the spin time");
    expect(trace[0].points[1] == at(250, 200, 50), "SP-05: its second point is the middle of the run");
    expect(trace[0].points[2].z >= at(0, 0, 50).z, "SP-05: the bow never points down");
    expect(trace[0].roll == units(20) && trace[2].roll == units(60), "SP-07: the roll grows by 20 degrees a frame");
    expect(trace.back().position.x > at(100, 0).x && trace.back().position.x < at(400, 0).x,
        "SP-06: it ends short of the straight run's end");
    expect(trace.back().travelled == units(295), "SP-06: 59 frames at 5 units a frame");
}

// SC-03 (SP-05, SP-06): a craft at rest builds no path and explodes at its first service.
void test_rest() {
    auto resting = tactical::start_spin(8, craft_type, empire, at(0, 0), tactical::CraftState{});
    expect(run(resting, 10, 5) == 1, "SC-03: a spin from rest ends at its first service");
}

// SC-04 (SP-04): a spin whose accumulated roll comes back to zero rebuilds its path from where
// it is: a craft killed at roll -40 rebuilds at its third service and explodes at the 62nd.
void test_rebuild() {
    auto banked = tactical::start_spin(9, craft_type, empire, at(0, 0), flying(units(-40)));
    expect(run(banked, 10, 200) == 62, "SC-04: a roll of -40 rebuilds the path at the third service");
}

[[nodiscard]] tactical::MotionTable motion(const Fixed chance) {
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
    craft.spin_away = spin(chance);
    table.squadrons.craft = {craft};
    table.squadrons.squadrons = {{squadron_type, {craft_type, craft_type, craft_type},
        {at(0, 0), at(-10, 10), at(-10, -10)}, units(1000), units(200), units(20)}};
    return table;
}

[[nodiscard]] tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {Fixed::from_raw(one / 5), Fixed::from_raw(one * 2 / 5), Fixed::from_raw(one / 3)};
    table.profiles = {tactical::DurabilityProfile{craft_type, units(90), units(5), false, {}}};
    return table;
}

[[nodiscard]] tactical::TacticalSetup setup() {
    tactical::TacticalSetup result;
    result.seed = seed;
    result.players = {{empire, 1, 1, tactical::player_flag_commandable}, {rebel, 2, 2, tactical::player_flag_commandable}};
    const auto unit = [](const eawr::sim::EntityId id, const tactical::TypeId type, const math::Vec3& position) {
        return tactical::UnitState{id, type, empire, position, math::identity_quat(), {}};
    };
    result.units = {unit(10, squadron_type, at(0, 0)), unit(11, craft_type, at(0, 0)), unit(12, craft_type, at(-10, 10)),
        unit(13, craft_type, at(-10, -10))};
    result.squadrons = {{10, {11, 12, 13}}};
    return result;
}

[[nodiscard]] std::vector<tactical::SensorProfile> sensors() {
    return {{craft_type, units(8000)}, {squadron_type, units(8000)}};
}

struct Timeline {
    std::optional<std::uint64_t> destroyed;
    std::optional<std::uint64_t> started;
    std::optional<std::uint64_t> ended;
    std::vector<std::uint64_t> spinning_ticks; // completed ticks whose snapshot shows craft 12 spinning
    std::optional<std::uint64_t> left_squadron; // first completed tick without 12 among its members
    std::vector<std::string> hashes;
};

// Craft 12 is killed by a scripted hit at tick 40; the session runs 140 ticks.
[[nodiscard]] Timeline kill_session(const Fixed chance, const std::size_t workers) {
    Timeline timeline;
    auto created = tactical::TacticalSession::create(setup(), sensors(), durability(), motion(chance));
    expect(static_cast<bool>(created), "SC-05: the session builds");
    if (!created) return timeline;
    auto session = std::move(created).value();
    const auto kill = tactical::PlayerCommand{{40, empire, 1}, {12}, tactical::DamagePayload{units(1000)}};
    expect(static_cast<bool>(session.submit(kill)), "SC-05: the kill submits");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < 140; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "SC-05: step failed: " + stepped.error().message);
            break;
        }
        const auto& snapshot = *stepped.value().snapshot;
        timeline.hashes.push_back(stepped.value().state_sha256 + snapshot.sha256());
        for (const auto& event : snapshot.events()) {
            if (event.unit != 12) continue;
            if (event.kind == tactical::EventKind::unit_destroyed) timeline.destroyed = event.tick;
            if (event.kind == tactical::EventKind::spin_away_started) timeline.started = event.tick;
            if (event.kind == tactical::EventKind::spin_away_ended) timeline.ended = event.tick;
        }
        for (const auto& spinning : snapshot.spinning()) {
            if (spinning.entity_id == 12) timeline.spinning_ticks.push_back(snapshot.completed_tick());
        }
        if (!timeline.left_squadron) {
            bool member = false;
            for (const auto& squadron : session.squadrons()) {
                for (const auto id : squadron.members) member = member || id == 12;
            }
            if (!member) timeline.left_squadron = snapshot.completed_tick();
        }
    }
    return timeline;
}

// SC-05 (SP-02, SP-08): the kill counts at once; the spin shows for two seconds, then explodes.
void test_session() {
    const auto spinning = kill_session(units(1), 1);
    expect(spinning.destroyed == 40, "SC-05: the kill is its unit_destroyed event at the hit's tick");
    expect(spinning.started == 40, "SC-05: spin_away_started in the same tick");
    expect(spinning.left_squadron == 41, "SC-05: the squadron loses the craft at once");
    expect(spinning.ended == 100, "SC-05: spin_away_ended 60 frames later");
    expect(spinning.spinning_ticks.size() == 60 && spinning.spinning_ticks.front() == 41
            && spinning.spinning_ticks.back() == 100,
        "SC-05: the snapshots of ticks 41 to 100 show it spinning");
    const auto exploding = kill_session(Fixed{}, 1);
    expect(exploding.destroyed == 40 && !exploding.started && !exploding.ended && exploding.spinning_ticks.empty(),
        "SC-05: without a spin the craft only dies");
    // The same session before the kill hashes alike: nothing spins before a death.
    expect(spinning.hashes.size() == 140 && exploding.hashes.size() == 140, "SC-05: 140 ticks");
    if (spinning.hashes.size() == 140 && exploding.hashes.size() == 140) {
        expect(std::vector(spinning.hashes.begin(), spinning.hashes.begin() + 40)
                == std::vector(exploding.hashes.begin(), exploding.hashes.begin() + 40),
            "SC-05: a spin changes no hash before the death");
    }
}

// SC-06: a spinning session hashes the same with 1, 2, 4 and 8 workers.
void test_worker_equality() {
    const auto reference = kill_session(units(1), 1).hashes;
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(kill_session(units(1), workers).hashes == reference,
            "SC-06: " + std::to_string(workers) + " workers hash like one");
    }
}

// SC-07: a craft killed while its squadron flies a move order, printed for the #472 eye check
// ("the spin looks in place"): in the session the copy leaves the kill point along the craft's
// flight for the whole spin.
struct MovingKill {
    std::vector<math::Vec3> flight;    // craft 12 in the snapshots before its death
    std::vector<math::Vec3> spin;      // the spinning copy, one per snapshot
    std::optional<std::uint64_t> started;
    std::optional<std::uint64_t> ended;
};

[[nodiscard]] math::Vec3 translation(const math::Mat3x4& transform) {
    return {transform.rows[0][3], transform.rows[1][3], transform.rows[2][3]};
}

[[nodiscard]] double to_units(const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(one); }

[[nodiscard]] double distance(const math::Vec3& from, const math::Vec3& to) {
    const double dx = to_units(to.x) - to_units(from.x);
    const double dy = to_units(to.y) - to_units(from.y);
    const double dz = to_units(to.z) - to_units(from.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

constexpr std::uint64_t moving_kill_tick = 90;

[[nodiscard]] MovingKill moving_kill_session() {
    MovingKill result;
    auto created = tactical::TacticalSession::create(setup(), sensors(), durability(), motion(units(1)));
    expect(static_cast<bool>(created), "SC-07: the session builds");
    if (!created) return result;
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.submit({{5, empire, 1}, {10}, tactical::MovePayload{at(6000, 0)}})),
        "SC-07: the move submits");
    expect(static_cast<bool>(session.submit({{moving_kill_tick, empire, 2}, {12}, tactical::DamagePayload{units(1000)}})),
        "SC-07: the kill submits");
    const eawr::platform::ThreadWorkerAdapter executor(1);
    for (int tick = 0; tick < 200; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "SC-07: step failed: " + stepped.error().message);
            break;
        }
        const auto& snapshot = *stepped.value().snapshot;
        for (const auto& instance : snapshot.instances()) {
            if (instance.entity_id == 12) result.flight.push_back(translation(instance.fixed_transform));
        }
        for (const auto& spinning : snapshot.spinning()) {
            if (spinning.entity_id == 12) result.spin.push_back(translation(spinning.fixed_transform));
        }
        for (const auto& event : snapshot.events()) {
            if (event.unit != 12) continue;
            if (event.kind == tactical::EventKind::spin_away_started) result.started = event.tick;
            if (event.kind == tactical::EventKind::spin_away_ended) result.ended = event.tick;
        }
    }
    return result;
}

void test_moving_kill() {
    const auto run = moving_kill_session();
    expect(run.started == moving_kill_tick && run.ended == moving_kill_tick + 60,
        "SC-07: the moving craft spins for 60 frames from its kill");
    expect(run.flight.size() >= 2 && run.spin.size() == 60, "SC-07: the craft flew, then spun in 60 snapshots");
    if (run.flight.size() < 2 || run.spin.size() != 60) return;
    const auto& killed_at = run.flight.back();
    const double speed = distance(run.flight[run.flight.size() - 2], killed_at);
    double path = 0.0;
    for (std::size_t index = 0; index < run.spin.size(); ++index) {
        path += distance(index == 0 ? killed_at : run.spin[index - 1], run.spin[index]);
    }
    const double reach = distance(killed_at, run.spin.back());
    std::cout << "SC-07: flight speed " << speed << " units/frame before the kill; the copy covers " << path
              << " units in 60 frames and ends " << reach << " units from the kill point (a straight run: "
              << speed * 60.0 << ")\n";
    expect(speed > 1.0, "SC-07: the craft is flying when it is killed");
    // Today (#472 check): 5.4 units a frame; the copy runs about 337 units of bowed path and ends
    // about 321 units away, just short of the 324-unit straight run (SP-06).
    expect(reach > speed * 54.0 && reach < speed * 60.0,
        "SC-07: the copy ends most of a straight run from the kill point, short of its end");
    expect(path > reach, "SC-07: its path bows, so it runs further than it ends away");
}

void test_validation() {
    auto table = motion(units(1));
    expect(static_cast<bool>(tactical::validate_squadron_table(table.squadrons)), "a spin-away table is valid");
    table.squadrons.craft[0].spin_away->chance = tenths(11);
    expect(!tactical::validate_squadron_table(table.squadrons), "a chance above 1 is rejected");
    table.squadrons.craft[0].spin_away = tactical::SpinAwayProfile{tenths(2), units(61)};
    expect(!tactical::validate_squadron_table(table.squadrons), "a spin time above 60 s is rejected");
}

} // namespace

int main() {
    test_chance();
    test_timing();
    test_rest();
    test_rebuild();
    test_session();
    test_worker_equality();
    test_moving_kill();
    test_validation();
    if (failures != 0) {
        std::cerr << failures << " fighter death contract failure(s)\n";
        return 1;
    }
    std::cout << "fighter death contracts passed\n";
    return 0;
}
