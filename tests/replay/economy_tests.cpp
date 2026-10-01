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
namespace {

namespace sim = eawr::sim;
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
[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}
[[nodiscard]] double real(const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(one); }

constexpr tactical::PlayerId human = 1;
constexpr tactical::PlayerId ai = 2;
constexpr tactical::FactionId rebel = 100;
constexpr tactical::FactionId empire = 200;
constexpr tactical::TypeId ship_type = 10;     // a corvette-like buy: 500, 450 frames, population 2
constexpr tactical::TypeId station_type = 40;
constexpr tactical::TypeId craft_type = 60;
constexpr tactical::TypeId squadron_type = 70; // three craft: 550, 510 frames, population 1
constexpr tactical::TypeId upgrade_type = 90;  // listed, never built (PU-20)
constexpr sim::EntityId human_station = 1;
constexpr sim::EntityId ai_station = 2;

[[nodiscard]] tactical::TacticalSetup setup() {
    tactical::TacticalSetup result;
    result.seed = 530;
    result.players = {{human, 0, rebel, tactical::player_flag_commandable}, {ai, 1, empire, tactical::player_flag_commandable}};
    result.units = {{human_station, station_type, human, at(-3000, 0), math::identity_quat(), {}},
        {ai_station, station_type, ai, at(3000, 0), math::identity_quat(), {}}};
    return result;
}

[[nodiscard]] tactical::EconomyRules rules(const std::int64_t credits = 6000, const std::uint32_t human_cap = 25) {
    tactical::EconomyRules result;
    result.players = {{human, units(credits), human_cap, false, Fixed{}}, {ai, units(credits), 20, true, units(180)}};
    const std::vector<tactical::BuildOption> options{
        {ship_type, tactical::BuildKind::unit, tactical::BuildQueue::units, units(500), 450, 450, 2, true},
        {squadron_type, tactical::BuildKind::unit, tactical::BuildQueue::units, units(550), 510, 510, 1, true},
        {upgrade_type, tactical::BuildKind::upgrade, tactical::BuildQueue::units, Fixed{}, 0, 0, 0, false}};
    result.menus = {{station_type, rebel, options}, {station_type, empire, options}};
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
    const tactical::EconomyRules& economy = rules(), const tactical::MotionTable& table = motion()) {
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

// PU-35: the lane table and its sum.
void test_arrival_table() {
    const auto ease = [](const double t) { return t <= 0.5 ? 4 * t / 3 : 2.0 / 3 + 8.0 / 3 * (t - t * t / 2 - 3.0 / 8); };
    double lane = 7500;
    for (int j = 1; j <= 10; ++j) lane += 750 - 741.875 * ease(j / 10.0);
    lane += 75 * 8.125;
    for (int j = 1; j <= 30; ++j) lane += 8.125 * (1 - ease(j / 30.0));
    expect(std::abs(real(tactical::arrival_tail(0)) - lane) < 1e-4, "PU-35: the lane is the sum of the table");
    expect(tactical::arrival_tail(0) == tactical::arrival_tail(24), "PU-35: nothing moves before frame 25");
    expect(tactical::arrival_tail(24).raw() - tactical::arrival_tail(25).raw() == units(750).raw(), "PU-35: frame 25 flies 750");
    expect(tactical::arrival_tail(44).raw() - tactical::arrival_tail(45).raw() == Fixed::from_raw(one * 65 / 8).raw(),
        "PU-35: frame 45 flies 8.125");
    expect(std::abs(real(Fixed::from_raw(tactical::arrival_tail(38).raw() - tactical::arrival_tail(39).raw()))
               - (750 - 741.875 * 2.0 / 3)) < 1e-6,
        "PU-35: frame 39 flies 750 - 741.875 E(1/2)");
    expect(tactical::arrival_tail(149).raw() == 0 && tactical::arrival_tail(150).raw() == 0, "PU-35: the lane ends at frame 149");
    bool falling = true;
    for (std::uint32_t frame = 1; frame < tactical::arrival_frames; ++frame) {
        falling = falling && tactical::arrival_tail(frame) <= tactical::arrival_tail(frame - 1);
    }
    expect(falling, "PU-35: the unit only ever flies forward");
    expect(tactical::population_count(0) == 0 && tactical::population_count(1) == 1
            && tactical::population_count(tactical::population_share_scale) == 1
            && tactical::population_count(tactical::population_share_scale + 1) == 2,
        "PU-21: a fraction rounds up");
    expect(3 * tactical::population_share(1, 3) == tactical::population_share_scale, "PU-21: three craft share one exactly");
}

// PC-01, PU-02 to PU-04: 5 credits a second each.
void test_income() {
    auto world = session();
    if (!world) return;
    step_to(*world, 300);
    const auto* mine = ledger(*world, human);
    const auto* theirs = ledger(*world, ai);
    expect(mine != nullptr && std::abs(real(mine->credits) - 6050) < 1e-3, "PC-01: 6000 + 300 frames x 1/6 = 6050");
    expect(theirs != nullptr && mine != nullptr && theirs->credits == mine->credits, "PC-01: each player earns its own station's");
    const auto views = world->snapshot()->economy();
    expect(views.size() == 2 && mine != nullptr && views[0].credits == mine->credits && views[0].population_cap == 25,
        "the snapshot carries the ledger");
    // PU-05: the stream ends with its station.
    expect(static_cast<bool>(world->stage_remove(human_station)), "the human station is removed");
    const auto before = ledger(*world, human)->credits;
    step_to(*world, 310);
    expect(ledger(*world, human)->credits == before, "PU-05: no station, no income");
}

// PC-02, PU-15, PU-16.
void test_buy() {
    auto world = session();
    if (!world) return;
    expect(static_cast<bool>(world->submit(buy(10, human, 0, human_station, ship_type))), "the buy is submitted");
    auto events = step_to(*world, 11);
    expect(!events.empty() && events.back().kind == tactical::EventKind::order_accepted
            && events.back().order == tactical::OrderKind::buy && events.back().unit == human_station,
        "PC-02: the buy is accepted");
    const auto after_buy = real(ledger(*world, human)->credits);
    expect(std::abs(after_buy - (6000 + 11.0 / 6 - 500)) < 1e-3, "PU-15: the price is paid when queued");
    step_to(*world, 460);
    expect(ledger(*world, human)->pool.empty(), "PU-16: not done before 450 frames");
    step_to(*world, 461);
    expect(ledger(*world, human)->pool == std::vector<tactical::TypeId>{ship_type}, "PC-02: done 450 frames after the buy");
    expect(ledger(*world, human)->queues[0].empty(), "the queue is empty again");
}

// PC-04, PU-11, PU-14, PU-15.
void test_refusals() {
    auto world = session();
    if (!world) return;
    for (std::uint64_t index = 0; index < 6; ++index) {
        expect(static_cast<bool>(world->submit(buy(0, human, index, human_station, ship_type))), "a human buy is submitted");
        expect(static_cast<bool>(world->submit(buy(0, ai, index, ai_station, ship_type))), "an AI buy is submitted");
    }
    expect(static_cast<bool>(world->submit(buy(1, human, 0, ai_station, ship_type))), "a buy at the enemy station");
    expect(static_cast<bool>(world->submit(buy(1, human, 1, human_station, upgrade_type))), "an upgrade buy");
    const auto events = step_to(*world, 2);
    std::size_t human_accepted = 0;
    std::size_t ai_accepted = 0;
    std::vector<tactical::RejectReason> reasons;
    for (const auto& event : events) {
        if (event.kind == tactical::EventKind::order_accepted) (event.player == human ? human_accepted : ai_accepted) += 1;
        if (event.kind == tactical::EventKind::order_rejected) reasons.push_back(event.reason);
    }
    expect(human_accepted == 5 && ai_accepted == 6, "PC-04: a human queues five, the AI more");
    expect(reasons == std::vector<tactical::RejectReason>{tactical::RejectReason::queue_full,
                          tactical::RejectReason::cannot_produce, tactical::RejectReason::cannot_produce},
        "PU-14, PU-11, PU-20: a full queue, an enemy station and an unavailable upgrade are refused");
    auto poor = session(rules(400));
    if (!poor) return;
    expect(static_cast<bool>(poor->submit(buy(0, human, 0, human_station, ship_type))), "a buy too dear is submitted");
    const auto refused = step_to(*poor, 1);
    expect(!refused.empty() && refused.back().reason == tactical::RejectReason::insufficient_credits
            && std::abs(real(ledger(*poor, human)->credits) - (400 + 1.0 / 6)) < 1e-3,
        "PU-15: too few credits: refused and nothing paid");
}

// PC-03, PU-17.
void test_cancel() {
    auto world = session();
    if (!world) return;
    expect(static_cast<bool>(world->submit(buy(0, human, 0, human_station, ship_type))), "buy a ship");
    expect(static_cast<bool>(world->submit(buy(0, human, 1, human_station, squadron_type))), "buy a squadron");
    expect(static_cast<bool>(world->submit(cancel(100, human, 0, 0))), "cancel the ship");
    expect(static_cast<bool>(world->submit(cancel(100, human, 1, 5))), "cancel a missing entry");
    const auto events = step_to(*world, 101);
    expect(events.size() >= 2 && events[events.size() - 2].kind == tactical::EventKind::order_accepted
            && events.back().reason == tactical::RejectReason::no_queue_entry,
        "PU-17: the cancel is accepted, a missing entry refused");
    expect(std::abs(real(ledger(*world, human)->credits) - (6000 + 101.0 / 6 - 550)) < 1e-3, "PC-03: the ship's 500 is refunded");
    step_to(*world, 610);
    expect(ledger(*world, human)->pool.empty(), "PC-03: the squadron is not done 509 frames after the cancel");
    step_to(*world, 611);
    expect(ledger(*world, human)->pool == std::vector<tactical::TypeId>{squadron_type},
        "PC-03: the squadron completes 510 frames after the cancel made it the front");
}

// PU-18: a lost station's entries go; only an AI is refunded.
void test_station_lost() {
    auto world = session();
    if (!world) return;
    expect(static_cast<bool>(world->submit(buy(0, human, 0, human_station, ship_type))), "the human buys");
    expect(static_cast<bool>(world->submit(buy(0, ai, 0, ai_station, ship_type))), "the AI buys");
    step_to(*world, 1);
    const auto human_before = ledger(*world, human)->credits;
    expect(static_cast<bool>(world->stage_remove(human_station)) && static_cast<bool>(world->stage_remove(ai_station)),
        "both stations are removed");
    step_to(*world, 2);
    expect(ledger(*world, human)->queues[0].empty() && ledger(*world, human)->credits == human_before,
        "PU-18: the human's entry goes without refund");
    expect(ledger(*world, ai)->queues[0].empty() && std::abs(real(ledger(*world, ai)->credits) - (6000 + 1.0 / 6)) < 1e-3,
        "PU-18: the AI gets its price back");
}

// PC-05, PU-30 to PU-39: a ship.
void test_reinforce_ship() {
    auto world = session();
    if (!world) return;
    expect(static_cast<bool>(world->submit(buy(0, human, 0, human_station, ship_type))), "buy a ship");
    step_to(*world, 451);
    expect(ledger(*world, human)->pool.size() == 1, "the ship is pooled");
    const auto point = at(-2000, 1000);
    const auto id = world->next_entity_id();
    expect(static_cast<bool>(world->submit(reinforce(451, human, 1, ship_type, point))), "bring it in");
    auto events = step_to(*world, 452);
    expect(!events.empty() && events.back().kind == tactical::EventKind::order_accepted && events.back().unit == id
            && events.back().order == tactical::OrderKind::reinforce,
        "PC-05: the reinforcement is accepted and names its unit");
    expect(ledger(*world, human)->pool.empty(), "PU-34: it leaves the pool");
    const auto views = world->snapshot()->economy();
    expect(!views.empty() && views[0].population == 2, "PU-34: it counts toward the population at once");
    const auto* created = instance(*world->snapshot(), id);
    expect(created != nullptr && created->arrival == 0U, "PU-35: arrival frame 0 in its first frame");
    expect(created != nullptr && created->visible_to == 1U, "PU-37: while hidden only its own team sees it");
    const auto lane = tactical::arrival_tail(0);
    const auto units_now = world->units();
    const auto self = std::find_if(units_now.begin(), units_now.end(), [id](const tactical::UnitState& unit) { return unit.entity_id == id; });
    expect(self != units_now.end() && self->position.x.raw() == point.x.raw() - lane.raw() && self->position.y == point.y,
        "PU-35: it starts D behind the point along its facing (yaw 0: +X)");
    expect(self != units_now.end() && self->position.z == units(-90),
        "LZ-01, PL-08: a bought single ship is created at its own height (Nebulon-B-like: -90) on the point, no search");
    expect(static_cast<bool>(world->submit({{460, human, 2}, {id}, tactical::StopPayload{}})), "an order during the arrival");
    events = step_to(*world, 461);
    expect(!events.empty() && events.back().reason == tactical::RejectReason::arriving, "PU-39: refused while arriving");
    step_to(*world, 486);
    created = instance(*world->snapshot(), id);
    expect(created != nullptr && created->arrival == 34U && created->visible_to == 1U, "PU-36: still hidden at frame 34");
    step_to(*world, 487);
    created = instance(*world->snapshot(), id);
    expect(created != nullptr && created->arrival == 35U && created->visible_to == 3U, "PU-36: shown from frame 35");
    step_to(*world, 601);
    expect(world->arrivals().size() == 1, "PU-35: still arriving at frame 149");
    const auto unloaded = step_to(*world, 602);
    expect(std::count_if(unloaded.begin(), unloaded.end(), [id](const tactical::Event& event) {
        return event.kind == tactical::EventKind::reinforcement_unloaded && event.unit == id;
    }) == 1, "WR-40: exactly one unloaded notification at frame 150");
    created = instance(*world->snapshot(), id);
    const auto landed = world->units();
    const auto rest = std::find_if(landed.begin(), landed.end(), [id](const tactical::UnitState& unit) { return unit.entity_id == id; });
    expect(created != nullptr && !created->arrival && world->arrivals().empty(), "PU-35: the arrival ends at frame 150");
    expect(rest != landed.end() && rest->position == math::Vec3{point.x, point.y, units(-90)},
        "PU-35, LZ-01: it rests on the point, raised by its type's Layer_Z_Adjust");
    expect(static_cast<bool>(world->submit({{602, human, 3}, {id}, tactical::StopPayload{}})), "an order after the arrival");
    events = step_to(*world, 603);
    expect(!events.empty() && events.back().kind == tactical::EventKind::order_accepted, "PU-39: accepted once arrived");
}

// PU-37, PU-38: how a projectile hit is taken while a unit arrives.
void test_arrival_hits() {
    auto value = rules();
    expect(tactical::arrival_hit_defense(nullptr, value) == Fixed{}, "a unit not arriving takes a hit as usual");
    tactical::ArrivalState state;
    for (const std::uint32_t frame : {0U, 34U}) {
        state.frame = frame;
        expect(!tactical::arrival_hit_defense(&state, value), "PU-37: a hit while hidden is dropped (frame " + std::to_string(frame) + ")");
    }
    for (const std::uint32_t frame : {35U, 149U}) {
        state.frame = frame;
        expect(tactical::arrival_hit_defense(&state, value) == units(-3),
            "PU-38: from frame 35 the hit takes the -3 vulnerability (frame " + std::to_string(frame) + ")");
    }
    // The window follows the rules' duration, not the arrival's end.
    value.vulnerability_frames = 60;
    state.frame = 59;
    expect(tactical::arrival_hit_defense(&state, value) == units(-3), "PU-38: vulnerable through vulnerability_frames - 1");
    state.frame = 60;
    expect(tactical::arrival_hit_defense(&state, value) == Fixed{}, "PU-38: not vulnerable from vulnerability_frames");
}

// WR-33/36/39/40: privileged script damage bypasses immunity; reveal and visibility differ.
void test_arrival_services() {
    auto initial = setup();
    initial.units.push_back({3, 99, ai, at(-1800, 0), math::identity_quat(), {}});
    const std::vector<tactical::SensorProfile> reveal{{ship_type, units(1100)}, {station_type, units(500)}};
    tactical::DurabilityTable health;
    health.profiles = {{ship_type, units(1000), std::nullopt, false, {}}};
    const tactical::FogRules fog{units(-6500), units(6500), units(100), 130, 130, 16, 21};
    auto content = rules();
    content.vulnerability_frames = 180; // WR-41: authored durations may outlive landing
    auto created = tactical::TacticalSession::create(initial, reveal, health, motion(), fog, {}, {}, {}, content);
    expect(static_cast<bool>(created), "WR-35: create arrival sensor/damage fixture");
    if (!created) return;
    auto world = std::move(created).value();
    expect(static_cast<bool>(world.submit(buy(0, human, 0, human_station, ship_type))), "buy arrival fixture ship");
    step_to(world, 451);
    const auto id = world.next_entity_id();
    expect(static_cast<bool>(world.submit(reinforce(451, human, 1, ship_type, at(-2600, 0)))), "reinforce in existing station reveal");
    step_to(world, 452);
    if (!instance(*world.snapshot(), id)) { expect(false, "arrival fixture ship exists"); return; }
    auto hull = world.durability_state(id)->hull;
    for (const auto& [frame, sequence] : {std::pair{0U, 2U}, std::pair{34U, 3U}, std::pair{35U, 4U}}) {
        const auto tick = std::uint64_t{451} + frame + (frame == 0 ? 1U : 0U);
        expect(static_cast<bool>(world.submit({{tick, human, sequence}, {id}, tactical::DamagePayload{units(10), tactical::hull_target}})),
            "submit boundary scripted damage");
        step_to(world, tick + 1);
        const auto health_now = world.durability_state(id);
        hull = math::Fixed::from_raw(hull.raw() - units(10).raw());
        expect(health_now && health_now->hull == hull,
            "WR-33: script damage bypasses arrival immunity at frames 0, 34 and 35");
    }
    step_to(world, 571); // snapshot arrival counter 119
    expect(!instance(*world.snapshot(), id)->reveal_range, "WR-35: arrival sensor disabled through 119");
    expect(!world.fog_cells()->revealed(0, at(-1800, 0)), "WR-35: early arrival never reveals its target cell");
    step_to(world, 572);
    expect(instance(*world.snapshot(), id)->reveal_range == units(1100), "WR-39: sensor enables at exactly 120");
    expect(world.fog_cells()->revealed(0, at(-1800, 0)), "WR-39: frame-120 revealer enters ordinary fog service");
    const auto landed = step_to(world, 602);
    expect(std::count_if(landed.begin(), landed.end(), [](const tactical::Event& event) {
        return event.kind == tactical::EventKind::reinforcement_unloaded;
    }) == 1, "WR-40: one completion event, no repeat");
    const std::array<std::uint8_t, 4> timer{'A', 'V', 'U', 'L'};
    const auto has_timer = [&](const tactical::TacticalSession& session) {
        const auto bytes = session.canonical_state_bytes();
        return std::search(bytes.begin(), bytes.end(), timer.begin(), timer.end()) != bytes.end();
    };
    expect(has_timer(world), "WR-41: AVUL survives frame 150 for longer authored duration");
    step_to(world, 632);
    expect(!has_timer(world), "WR-41: independent timer expires at its authored frame 180");

    auto doomed = tactical::TacticalSession::create(initial, reveal, health, motion(), fog, {}, {}, {}, content);
    expect(static_cast<bool>(doomed), "WR-40: create destroyed-arrival fixture");
    if (!doomed) return;
    auto destroyed = std::move(doomed).value();
    expect(static_cast<bool>(destroyed.submit(buy(0, human, 0, human_station, ship_type))), "buy doomed ship");
    step_to(destroyed, 451);
    const auto doomed_id = destroyed.next_entity_id();
    expect(static_cast<bool>(destroyed.submit(reinforce(451, human, 1, ship_type, at(-2600, 0)))), "reinforce doomed ship");
    step_to(destroyed, 452);
    expect(static_cast<bool>(destroyed.submit({{452, human, 2}, {doomed_id},
        tactical::DamagePayload{units(1000), tactical::hull_target}})), "privileged damage destroys hidden arrival");
    const auto after_death = step_to(destroyed, 602);
    expect(!instance(*destroyed.snapshot(), doomed_id) && destroyed.arrivals().empty(), "destroyed arrival retires its state");
    expect(std::none_of(after_death.begin(), after_death.end(), [](const tactical::Event& event) {
        return event.kind == tactical::EventKind::reinforcement_unloaded;
    }), "WR-40: a destroyed arrival emits no unloaded notification");
}

void test_pending_victory_reinforcement() {
    tactical::DurabilityTable health;
    health.profiles = {{station_type, units(100), std::nullopt, false, {}}};
    const tactical::VictoryRules victory{tactical::VictoryCondition::enemy_starbase_destroyed, {station_type}, {human, ai}, {human}};
    auto created = tactical::TacticalSession::create(setup(), sensors, health, motion(), std::nullopt, {}, victory, {}, rules());
    expect(static_cast<bool>(created), "WR-19: create victory fixture");
    if (!created) return;
    auto world = std::move(created).value();
    expect(static_cast<bool>(world.submit(buy(0, human, 0, human_station, ship_type))), "buy before decision");
    step_to(world, 451);
    expect(static_cast<bool>(world.submit({{451, ai, 0}, {ai_station}, tactical::DamagePayload{units(100), tactical::hull_target}})), "decide battle");
    step_to(world, 452);
    expect(world.outcome().has_value(), "victory is pending");
    const auto next = world.next_entity_id();
    const auto pool = ledger(world, human)->pool;
    expect(static_cast<bool>(world.submit(reinforce(452, human, 1, ship_type, at(-2000, 1000)))), "enqueue preselected arrival after decision");
    const auto events = step_to(world, 453);
    expect(events.size() == 1 && events[0].reason == tactical::RejectReason::battle_decided, "WR-19: pending victory drops arrival");
    expect(world.next_entity_id() == next && ledger(world, human)->pool == pool && world.arrivals().empty(), "WR-19: pool and population unchanged");
}

[[nodiscard]] tactical::MotionTable arrival_lane_motion() {
    auto table = motion();
    table.avoidance = tactical::AvoidanceRules{units(24), Fixed::from_raw(one / 5), units(100), Fixed::from_raw(one * 4 / 5), units(15),
        Fixed::from_raw(one * 66 / 100), Fixed::from_raw(one * 12 / 10), Fixed::from_raw(one / 4), Fixed::from_raw(one * 17 / 10),
        Fixed::from_raw(one / 2), Fixed::from_raw(one / 2), 3500, 6, 90, 45, units(50)};
    table.footprints = {{ship_type, tactical::SpaceLayer::corvette, units(10), units(20), units(20), false},
        {station_type, tactical::SpaceLayer::corvette, units(10), units(20), units(20), true},
        {craft_type, tactical::SpaceLayer::none, units(3), units(4), units(4), false}};
    return table;
}

void test_arrival_lane_sweep() {
    auto table = arrival_lane_motion();
    // An obstacle behind the requested point is on the approach sweep, clear of the endpoint.
    auto world = session(rules(), table);
    if (!world) return;
    auto verdict = world->reinforcement_point(human, ship_type, at(-2850, 0));
    expect(verdict && !verdict.value(), "WR-25: blocker behind endpoint rejects 200-unit sweep");
    verdict = world->reinforcement_point(human, ship_type, at(-2850, 60));
    expect(verdict && verdict.value(), "WR-25: hard rectangular Y extents leave adjacent lane clear");
    verdict = world->reinforcement_point(human, squadron_type, at(-2850, 0));
    expect(verdict && verdict.value(), "WR-27: layer-less squadron ignores corvette collisions");
    table.footprints[1].layer = tactical::SpaceLayer::static_object;
    auto statics = session(rules(), table);
    if (!statics) return;
    verdict = statics->reinforcement_point(human, squadron_type, at(-2850, 0));
    expect(verdict && !verdict.value(), "WR-28: same formation sweep still rejects a static obstruction");
    table.footprints[0].layer = tactical::SpaceLayer::none;
    auto unlayered = session(rules(), table);
    if (!unlayered) return;
    verdict = unlayered->reinforcement_point(human, ship_type, at(-3000, 0));
    expect(verdict && verdict.value(), "WR-24: no-layer single ship bypasses even the static layer");

    // WR-25: use the moving object's prediction across the full future arrival window.
    table.footprints[0].layer = tactical::SpaceLayer::corvette;
    tactical::MotionProfile mobile;
    mobile.type_id = ship_type;
    mobile.max_speed = units(5);
    mobile.acceleration = units(1);
    mobile.deceleration = units(1);
    mobile.rate_of_turn = units(5);
    mobile.turn_in_place_slowdown = units(1);
    table.profiles = {mobile};
    table.rules.arc_degrees = units(15);
    table.rules.expansion_distance = units(50);
    auto initial = setup();
    initial.units[0].position = at(-6000, 0);
    const auto facing = tactical::yaw_rotation(units(270));
    expect(static_cast<bool>(facing), "moving blocker facing");
    if (!facing) return;
    initial.units.push_back({3, ship_type, human, at(-3000, 400), facing.value(), {}});
    auto moving = tactical::TacticalSession::create(initial, sensors, {}, table, std::nullopt, {}, {}, {}, rules());
    expect(static_cast<bool>(moving), "moving lane fixture");
    if (!moving) return;
    auto moving_world = std::move(moving).value();
    expect(static_cast<bool>(moving_world.submit({{0, human, 0}, {3}, tactical::MovePayload{at(-3000, -400)}})), "move blocker across lane");
    step_to(moving_world, 5);
    tactical::TacticalSession::PlacementWork work;
    verdict = moving_world.reinforcement_point(human, ship_type, at(-2930, 0), &work);
    expect(verdict && !verdict.value(), "WR-25: future crossing rejects a presently clear lane");
    expect(work.predictions == 3, "WR-25: one mover samples only three horizon boundaries, never all 46");
}

// WR-25: preview and admission use current tracking windows, including after a command rebuild.
void test_arrival_tracking_frames() {
    for (const std::uint32_t interval : {45U, 90U}) {
        auto table = arrival_lane_motion();
        table.avoidance->tracking_interval = interval;
        tactical::MotionProfile mobile;
        mobile.type_id = ship_type;
        mobile.max_speed = units(5);
        mobile.acceleration = units(1);
        mobile.deceleration = units(1);
        mobile.rate_of_turn = units(5);
        mobile.turn_in_place_slowdown = units(1);
        table.profiles = {mobile};
        table.rules.arc_degrees = units(15);
        table.rules.expansion_distance = units(50);
        auto economy = rules();
        for (auto& menu : economy.menus) menu.options[0].build_frames = 1;
        // Command ticks straddle both intervals; tick 539 admits into staged frame 540.
        for (const std::uint64_t tick : {44ULL, 45ULL, 46ULL, 89ULL, 90ULL, 91ULL, 539ULL, 4101ULL}) {
            for (const bool rebuild : {false, true}) {
                for (const bool blocked : {false, true}) {
                    auto initial = setup();
                    initial.units.push_back({3, ship_type, human, at(-5000, 1500), math::identity_quat(), {}});
                    auto created = tactical::TacticalSession::create(initial, sensors, {}, table, std::nullopt, {}, {}, {}, economy);
                    expect(static_cast<bool>(created), "WR-25: tracking boundary fixture");
                    if (!created) continue;
                    auto world = std::move(created).value();
                    expect(static_cast<bool>(world.submit(buy(0, human, 0, human_station, ship_type))), "WR-25: buy boundary reserve");
                    step_to(world, tick);
                    const auto point = at(-2850, blocked ? 0 : 60);
                    const auto state = world.canonical_state_bytes();
                    tactical::TacticalSession::PlacementWork work;
                    const auto preview = world.reinforcement_point(human, ship_type, point, &work);
                    expect(preview && preview.value() == !blocked, "WR-25: preview retains blocked and clear lane verdicts across rolling windows");
                    expect(world.canonical_state_bytes() == state, "WR-25: preview leaves authoritative state unchanged");
                    expect(work.predictions > 0 && work.predictions <= (interval == 90 ? 4U : 5U),
                        "WR-25: prediction work stays bounded by sweep length after the authored horizon");
                    if (rebuild) {
                        expect(static_cast<bool>(world.submit({{tick, human, 0}, {3}, tactical::MovePayload{at(-4900, 1500)}})),
                            "WR-25: earlier move rebuilds the queried layer");
                    }
                    expect(static_cast<bool>(world.submit(reinforce(tick, human, 1, ship_type, point))), "WR-25: submit matching admission query");
                    const auto events = step_to(world, tick + 1);
                    const auto event = std::find_if(events.begin(), events.end(), [](const auto& value) {
                        return value.order == tactical::OrderKind::reinforce;
                    });
                    expect(event != events.end() && event->tick == tick && event->kind == (blocked
                        ? tactical::EventKind::order_rejected : tactical::EventKind::order_accepted),
                        "WR-25: admission agrees with preview and retains command event tick");
                    expect(event != events.end() && event->reason == (blocked
                        ? tactical::RejectReason::invalid_position : tactical::RejectReason::none),
                        "WR-25: blocked lane rejection reason");
                    expect(world.arrivals().size() == (blocked ? 0U : 1U)
                        && ledger(world, human)->pool.size() == (blocked ? 1U : 0U),
                        "WR-25: blocked lane preserves reserve; clear lane creates one arrival");
                }
            }
        }
    }
}

// PU-31 to PU-33.
void test_reinforce_refusals() {
    auto world = session(rules(6000, 1));
    if (!world) return;
    expect(static_cast<bool>(world->submit(reinforce(0, human, 0, ship_type, at(0, 0)))), "an empty pool");
    expect(static_cast<bool>(world->submit(buy(0, human, 1, human_station, ship_type))), "buy a ship");
    expect(static_cast<bool>(world->submit(buy(0, human, 2, human_station, squadron_type))), "buy a squadron");
    const auto first = step_to(*world, 1);
    const auto empty = std::find_if(first.begin(), first.end(),
        [](const tactical::Event& event) { return event.order == tactical::OrderKind::reinforce; });
    expect(empty != first.end() && empty->reason == tactical::RejectReason::not_in_pool, "PU-33: nothing pooled, dropped");
    step_to(*world, 961);
    expect(ledger(*world, human)->pool.size() == 2, "both are pooled");
    expect(static_cast<bool>(world->submit(reinforce(961, human, 3, ship_type, at(-2000, 0)))), "a ship over the cap");
    expect(static_cast<bool>(world->submit(reinforce(961, human, 4, squadron_type, at(1500, 0)))), "near the enemy station");
    expect(static_cast<bool>(world->submit(reinforce(961, human, 5, squadron_type, at(-7000, 0)))), "off the map");
    expect(static_cast<bool>(world->submit(reinforce(961, human, 6, squadron_type, at(-2000, 0)))), "a valid squadron");
    const auto events = step_to(*world, 962);
    std::vector<tactical::RejectReason> reasons;
    for (const auto& event : events) {
        if (event.order == tactical::OrderKind::reinforce) reasons.push_back(event.reason);
    }
    expect(reasons == std::vector<tactical::RejectReason>{tactical::RejectReason::no_population_room,
                          tactical::RejectReason::invalid_position, tactical::RejectReason::invalid_position,
                          tactical::RejectReason::none},
        "PU-33: no room, inside a prevention circle and out of bounds are dropped");
    expect(ledger(*world, human)->pool == std::vector<tactical::TypeId>{ship_type}, "PU-33: a dropped unit stays pooled");
    // PU-34: a squadron arrives as one: three craft, then its team container, all arriving.
    // The snapshot's spans end with the next step: keep the leader's ID.
    const auto squadrons = world->snapshot()->squadrons();
    expect(!squadrons.empty() && squadrons.back().members.size() == 3, "PU-34: the squadron is registered with its craft");
    const auto leader_id = !squadrons.empty() && !squadrons.back().members.empty() ? squadrons.back().members.front()
                                                                                    : eawr::sim::invalid_entity_id;
    expect(world->arrivals().size() == 4, "PU-34: its craft and container arrive");
    step_to(*world, 1112);
    const auto rest = world->units();
    const auto leader = std::find_if(rest.begin(), rest.end(),
        [&](const tactical::UnitState& unit) { return unit.entity_id == leader_id; });
    expect(world->arrivals().empty() && leader != rest.end() && leader->position.x == units(-2000),
        "PU-35: the craft end their lane on the point");
    const auto views = world->snapshot()->economy();
    expect(!views.empty() && views[0].population == 1, "PU-21: the squadron counts its value once");
}

// PL-08, LZ-01: a bought squadron's craft are searched one after another from the point, so the
// first takes the point and the others the next free candidates; none overlaps another's box, and
// each is raised by its own height. A single ship skips the search and so overlaps what is there.
void test_arrival_placement() {
    auto world = session(rules(6000, 25));
    if (!world) return;
    expect(static_cast<bool>(world->submit(buy(0, human, 0, human_station, squadron_type))), "buy a squadron");
    expect(static_cast<bool>(world->submit(buy(0, human, 1, human_station, ship_type))), "buy a ship");
    step_to(*world, 961);
    const auto point = at(-2000, 500);
    const auto next_id = world->next_entity_id();
    expect(static_cast<bool>(world->submit(reinforce(961, human, 2, squadron_type, point))), "bring the squadron in");
    step_to(*world, 962);
    const auto squadrons = world->snapshot()->squadrons();
    expect(!squadrons.empty() && squadrons.back().members.size() == 3, "the squadron has its craft");
    if (squadrons.empty() || squadrons.back().members.size() != 3) return;
    const auto members = squadrons.back().members;
    expect(members.front() == next_id, "the craft are created in member order");
    step_to(*world, 1112); // the lane ends at frame 150: each craft rests where its search put it
    const auto rest = world->units();
    std::vector<math::Vec3> spots;
    for (const auto member : members) {
        const auto found = std::find_if(rest.begin(), rest.end(),
            [member](const tactical::UnitState& unit) { return unit.entity_id == member; });
        expect(found != rest.end(), "a craft landed");
        if (found != rest.end()) spots.push_back(found->position);
    }
    expect(spots.size() == 3 && spots[0].x == point.x && spots[0].y == point.y,
        "PL-08: the first craft takes the arrival point, the search starts there");
    bool apart = spots.size() == 3;
    bool raised = apart;
    for (std::size_t first = 0; first < spots.size(); ++first) {
        raised = raised && spots[first].z == units(40);
        for (std::size_t second = first + 1; second < spots.size(); ++second) {
            const auto dx = spots[first].x.raw() - spots[second].x.raw();
            const auto dy = spots[first].y.raw() - spots[second].y.raw();
            // Two boxes of half extent 20 overlap when both axes are closer than 40.
            apart = apart && (std::llabs(dx) >= units(40).raw() || std::llabs(dy) >= units(40).raw());
        }
    }
    expect(apart, "PL-04: no craft's placement box overlaps another's");
    expect(raised, "LZ-01: each craft is raised by its own Layer_Z_Adjust");
}

// Determinism: one script, every executor, scrambled storage and a replay round trip.
void test_determinism() {
    tactical::TacticalReplay replay;
    replay.setup = setup();
    replay.final_tick_count = 1200;
    replay.commands = {buy(0, human, 0, human_station, squadron_type), buy(0, ai, 0, ai_station, ship_type),
        buy(5, human, 1, human_station, ship_type), cancel(40, human, 2, 1), buy(41, ai, 1, ai_station, squadron_type),
        reinforce(520, human, 3, squadron_type, at(-2500, 1500)), reinforce(520, ai, 2, ship_type, at(2500, -1500)),
        reinforce(1000, ai, 3, squadron_type, at(2500, 1500))};
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
}

} // namespace

int main() {
    test_arrival_table();
    test_income();
    test_buy();
    test_refusals();
    test_cancel();
    test_station_lost();
    test_reinforce_ship();
    test_arrival_hits();
    test_arrival_services();
    test_pending_victory_reinforcement();
    test_arrival_lane_sweep();
    test_arrival_tracking_frames();
    test_reinforce_refusals();
    test_arrival_placement();
    test_determinism();
    test_validation();
    if (failures != 0) {
        std::cerr << failures << " economy check(s) failed\n";
        return 1;
    }
    std::cout << "economy contracts passed\n";
    return 0;
}
