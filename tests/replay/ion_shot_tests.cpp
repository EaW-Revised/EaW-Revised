#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #561: the Y-wing squadron's ION_CANNON_SHOT (docs/behaviour/space-abilities.md AB-60 to AB-69):
// a targeted team ability whose craft each fire one ion bolt at the target, holding every other
// shot meanwhile, then recharge; autofire for a player the engine plays; the targeted command's
// replay round trip; identical hashes at 1, 2, 4 and 8 workers.
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
constexpr tactical::PlayerId rebel = 1;
constexpr tactical::PlayerId empire = 2;
constexpr tactical::TypeId squadron_type = 100; // the Y-wing squadron (its team container)
constexpr tactical::TypeId craft_type = 101;    // a Y-wing
constexpr tactical::TypeId frigate_type = 102;  // an ion-stunnable, shielded target
constexpr eawr::sim::EntityId container = 10;
constexpr eawr::sim::EntityId craft_a = 11;
constexpr eawr::sim::EntityId craft_b = 12;
constexpr eawr::sim::EntityId frigate = 30;
constexpr eawr::sim::EntityId escort = 31; // a Rebel frigate: not a valid target for the Rebels

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}

[[nodiscard]] tactical::MotionTable motion() {
    tactical::MotionTable table;
    tactical::CraftProfile craft;
    craft.type_id = craft_type;
    craft.max_speed = Fixed::from_raw(one * 3);
    craft.min_speed = Fixed::from_raw(one * 2);
    craft.rate_of_turn = units(6);
    craft.lift = units(6);
    craft.thrust = Fixed::from_raw(one);
    craft.roll_rate = units(3);
    craft.bank_angle = units(70);
    craft.strafe_distance = units(500);
    table.squadrons.craft = {craft};
    table.squadrons.squadrons = {
        {squadron_type, {craft_type, craft_type}, {at(0, 0), at(-10, 10)}, units(1000), units(200), units(300), units(25)}};
    return table;
}

[[nodiscard]] tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {Fixed::from_raw(one / 5), Fixed::from_raw(one * 2 / 5), Fixed::from_raw(one / 3)};
    tactical::DamageRules rules;
    rules.shield_recharge_frames = 90;
    rules.depleted_disable_seconds = units(5);
    rules.depleted_regen_cap = Fixed::from_raw(one / 4);
    rules.diminishing = {{units(0), units(1)}, {units(2), units(1)}};
    rules.damage_types = 1;
    rules.armor_types = 1;
    rules.armor_mods = {units(1)};
    rules.energy_recharge_frames = 150;
    rules.energy_to_shield = units(5);
    table.damage = rules;
    tactical::DurabilityProfile craft{craft_type, units(60), units(3), false, {}};
    tactical::DurabilityProfile ship{frigate_type, units(3000), units(1), false, {}};
    ship.max_shields = units(700);
    ship.shield_refresh = units(10);
    ship.ion_stun_effect = true;
    table.profiles = {craft, ship};
    return table;
}

// The craft's laser hardpoint (range 500, one shot a second); under the ion shot it fires a 50-point
// bolt that damages shields only and stuns for 30 frames.
[[nodiscard]] tactical::CombatTable combat() {
    tactical::CombatTable table;
    tactical::CombatProfile craft;
    craft.type_id = craft_type;
    craft.category_bits = 1;
    craft.max_attack_distance = units(500);
    tactical::WeaponProfile laser;
    laser.hardpoint = 0;
    laser.range = units(500);
    laser.cone_width = units(360); // a full cone: the craft fire whichever way they face (W-09)
    laser.cone_height = units(180);
    laser.min_recharge_hundredths = 100;
    laser.max_recharge_hundredths = 100;
    laser.pulse_count = 2;
    laser.pulse_delay_frames = 6;
    laser.opportunity_when_idle = true;
    laser.opportunity_when_targeting = true;
    laser.shot = tactical::ShotProfile{units(5), 0, units(25), units(500), true, true, {}};
    laser.ability_shot = tactical::ShotProfile{units(50), 0, units(25), units(500), true, false, {}};
    laser.ability_shot->ion_stun = tactical::IonStunShot{30, Fixed::from_raw(one / 2), Fixed::from_raw(one / 2), true};
    craft.weapons = {laser};
    craft.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
    tactical::CombatProfile ship;
    ship.type_id = frigate_type;
    ship.category_bits = 2;
    ship.collision = tactical::CollisionBox{at(-60, -30, -30), at(60, 30, 30)};
    table.profiles = {craft, ship};
    return table;
}

[[nodiscard]] tactical::AbilityTable abilities(const std::vector<tactical::PlayerId>& humans) {
    tactical::AbilityTable table;
    tactical::AbilityProfile team;
    team.kind = tactical::AbilityKind::ion_cannon_shot;
    team.recharge_frames = 600;
    team.supports_autofire = true;
    team.team = true;
    tactical::AbilityProfile own;
    own.kind = tactical::AbilityKind::ion_cannon_shot;
    table.profiles = {{squadron_type, {team}, false}, {craft_type, {own}, false}};
    table.humans = humans;
    return table;
}

[[nodiscard]] std::vector<tactical::SensorProfile> sensors() {
    return {{squadron_type, units(8000)}, {craft_type, units(8000)}, {frigate_type, units(8000)}};
}

[[nodiscard]] tactical::UnitState unit(const eawr::sim::EntityId id, const tactical::TypeId type,
    const tactical::PlayerId owner, const math::Vec3& position) {
    return tactical::UnitState{id, type, owner, position, math::identity_quat(), {}};
}

[[nodiscard]] tactical::TacticalSetup setup() {
    tactical::TacticalSetup value;
    value.seed = 5611;
    value.players = {{rebel, 1, 1, tactical::player_flag_commandable}, {empire, 2, 2, tactical::player_flag_commandable}};
    value.units = {unit(container, squadron_type, rebel, at(-600, 0)), unit(craft_a, craft_type, rebel, at(-600, 0)),
        unit(craft_b, craft_type, rebel, at(-610, 10)), unit(frigate, frigate_type, empire, at(300, 0)),
        unit(escort, frigate_type, rebel, at(-800, 300))};
    value.squadrons = {{container, {craft_a, craft_b}}};
    return value;
}

[[nodiscard]] tactical::PlayerCommand shot(const std::uint64_t tick, const std::uint64_t sequence, const eawr::sim::EntityId unit,
    const eawr::sim::EntityId target, const tactical::AbilityAction action = tactical::AbilityAction::activate) {
    tactical::AbilityPayload payload{tactical::AbilityKind::ion_cannon_shot, action};
    payload.target = target;
    return {{tick, rebel, sequence}, {unit}, payload};
}

struct Run {
    std::vector<std::string> rows;
    std::vector<tactical::CombatEvent> fired;  // the craft's weapon_fired events
    std::vector<tactical::Event> events;       // order events
    std::vector<std::uint64_t> active_ticks;   // ticks the container's ion shot showed on
    std::optional<std::uint64_t> recharging_from;
    std::uint32_t recharge_total{};
    std::size_t stunned{};                      // ticks the frigate showed stunned
    std::optional<tactical::DurabilityState> target;
};

Run run(const std::vector<tactical::PlayerCommand>& commands, const std::vector<tactical::PlayerId>& humans,
    const eawr::sim::PartitionExecutor& executor, const std::uint64_t ticks = 600) {
    Run result;
    auto created = tactical::TacticalSession::create(setup(), sensors(), durability(), motion(), std::nullopt, combat(),
        tactical::VictoryRules{}, abilities(humans));
    expect(static_cast<bool>(created), "the ion shot session builds: " + (created ? std::string{} : created.error().message));
    if (!created) return result;
    auto session = std::move(created).value();
    for (const auto& command : commands) {
        const auto submitted = session.submit(command);
        expect(static_cast<bool>(submitted), "a command submits");
    }
    for (std::uint64_t index = 0; index < ticks; ++index) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "step failed: " + stepped.error().message);
            break;
        }
        const auto& snapshot = *stepped.value().snapshot;
        result.rows.push_back(stepped.value().state_sha256 + ',' + snapshot.sha256());
        for (const auto& event : snapshot.combat_events()) {
            if (event.kind == tactical::CombatEventKind::weapon_fired && (event.shooter == craft_a || event.shooter == craft_b)) {
                result.fired.push_back(event);
            }
        }
        for (const auto& event : snapshot.events()) {
            if (event.kind == tactical::EventKind::order_accepted || event.kind == tactical::EventKind::order_rejected) {
                result.events.push_back(event);
            }
        }
        for (const auto& instance : snapshot.instances()) {
            if (instance.entity_id == frigate && instance.ion_stun_frames != 0) ++result.stunned;
            if (instance.entity_id != container) continue;
            for (const auto& status : instance.abilities) {
                if (status.kind != tactical::AbilityKind::ion_cannon_shot) continue;
                if (status.active) result.active_ticks.push_back(stepped.value().completed_tick);
                if (!status.active && status.remaining_frames > 0 && !result.recharging_from) {
                    result.recharging_from = stepped.value().completed_tick;
                    result.recharge_total = status.total_frames;
                }
            }
        }
    }
    result.target = session.durability_state(frigate);
    return result;
}

[[nodiscard]] std::size_t bolts(const Run& value) {
    return static_cast<std::size_t>(std::count_if(value.fired.begin(), value.fired.end(),
        [](const tactical::CombatEvent& event) { return (event.outcome & tactical::fired_ability_shot) != 0U; }));
}

// AB-61 to AB-67: the player aims the ion shot at the frigate.
void test_player_shot() {
    const eawr::sim::InlineExecutor inline_executor;
    const auto value = run({shot(1, 1, container, frigate)}, {rebel}, inline_executor);
    expect(!value.events.empty() && value.events.front().kind == tactical::EventKind::order_accepted,
        "AB-62: the ion shot at an enemy frigate is accepted");
    expect(bolts(value) == 2, "AB-66, AB-67: each of the two Y-wings fires one ion bolt (" + std::to_string(bolts(value)) + ")");
    // AB-66: while the shot is on, the craft fire nothing else.
    const auto first_bolt = std::find_if(value.fired.begin(), value.fired.end(),
        [](const tactical::CombatEvent& event) { return (event.outcome & tactical::fired_ability_shot) != 0U; });
    bool held = true;
    if (first_bolt != value.fired.end() && !value.active_ticks.empty()) {
        for (const auto& event : value.fired) {
            const bool during = event.tick >= 1 && event.tick < value.active_ticks.back();
            held = held && (!during || (event.outcome & tactical::fired_ability_shot) != 0U);
        }
    }
    expect(held, "AB-66: no laser shot while the ion shot is on");
    for (const auto& event : value.fired) {
        if ((event.outcome & tactical::fired_ability_shot) != 0U) {
            expect(event.target == frigate, "AB-66: an ion bolt flies at the target");
        }
    }
    expect(value.stunned > 0, "IS-01: the bolts stun the frigate (" + std::to_string(value.stunned) + " ticks)");
    expect(value.target && value.target->shields < units(700) && value.target->hull.raw() == 3000 * one,
        "the bolts take shield and leave the hull");
    expect(value.recharging_from && value.recharge_total == 600,
        "AB-65: after both bolts the container recharges for 600 frames");
}

// AB-62: rejections.
void test_rejections() {
    const eawr::sim::InlineExecutor inline_executor;
    const auto reason_of = [&](const tactical::PlayerCommand& command) {
        const auto value = run({command}, {rebel}, inline_executor, 3);
        return value.events.empty() ? tactical::RejectReason::none : value.events.front().reason;
    };
    expect(reason_of(shot(1, 1, container, escort)) == tactical::RejectReason::target_not_hostile,
        "AB-62: an own-team target is refused");
    expect(reason_of(shot(1, 1, container, 999)) == tactical::RejectReason::target_not_live, "AB-62: a dead target is refused");
    expect(reason_of(shot(1, 1, craft_a, frigate)) == tactical::RejectReason::ability_unavailable,
        "AB-60: a craft cannot switch the team ability on itself");
    // A second shot while the first is on, and one while it recharges.
    const auto twice = run({shot(1, 1, container, frigate), shot(2, 2, container, frigate)}, {rebel}, inline_executor, 4);
    expect(twice.events.size() >= 2 && twice.events[1].reason == tactical::RejectReason::ability_unavailable,
        "AB-62: an ion shot that is on refuses another");
    const auto cancelled = run({shot(1, 1, container, frigate),
                                   shot(2, 2, container, eawr::sim::invalid_entity_id, tactical::AbilityAction::deactivate),
                                   shot(3, 3, container, frigate)},
        {rebel}, inline_executor, 5);
    expect(cancelled.events.size() >= 3 && cancelled.events[2].reason == tactical::RejectReason::none,
        "AB-65: cancelled before any craft fired, the shot costs no recharge");
}

// AB-68: a player the engine plays uses the ion shot on its squadron's attack target by itself; a
// human player's squadron without autofire never does.
void test_autofire() {
    const eawr::sim::InlineExecutor inline_executor;
    const tactical::PlayerCommand attack{{1, rebel, 1}, {container}, tactical::AttackPayload{frigate}};
    const auto engine = run({attack}, {}, inline_executor);
    const auto human = run({attack}, {rebel}, inline_executor);
    expect(bolts(engine) == 2, "AB-68: the engine's squadron fires its ion bolts at its attack target");
    expect(bolts(human) == 0 && !human.fired.empty(), "AB-68: a human's squadron without autofire only fires its lasers");
    tactical::PlayerCommand autofire{{1, rebel, 2}, {container},
        tactical::AbilityPayload{tactical::AbilityKind::ion_cannon_shot, tactical::AbilityAction::autofire_on}};
    const auto switched = run({attack, autofire}, {rebel}, inline_executor);
    expect(bolts(switched) == 2, "AB-68: a human's squadron on autofire fires its ion bolts too");
}

// The targeted command survives a written and parsed replay; malformed ones are rejected.
void test_replay() {
    tactical::TacticalReplay replay;
    replay.setup = setup();
    replay.final_tick_count = 10;
    replay.commands = {shot(1, 1, container, frigate)};
    std::get<tactical::AbilityPayload>(replay.commands[0].payload).target_hardpoint = 0;
    const auto bytes = tactical::write_replay(replay);
    expect(static_cast<bool>(bytes), "a replay with a targeted ability writes");
    if (bytes) {
        const auto parsed = tactical::parse_replay(bytes.value());
        expect(parsed && parsed.value() == replay, "a targeted ability command round-trips");
    }
    auto untargeted = replay;
    std::get<tactical::AbilityPayload>(untargeted.commands[0].payload).target = eawr::sim::invalid_entity_id;
    std::get<tactical::AbilityPayload>(untargeted.commands[0].payload).target_hardpoint = 0xffffffffU;
    expect(!tactical::validate_replay(untargeted), "AB-61: an ion shot without a target is rejected");
}

// The player's shot hashes the same at every worker count.
void test_workers() {
    const eawr::sim::InlineExecutor inline_executor;
    const auto reference = run({shot(1, 1, container, frigate)}, {rebel}, inline_executor, 300);
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(run({shot(1, 1, container, frigate)}, {rebel}, executor, 300).rows == reference.rows,
            "ion shot hashes match with " + std::to_string(workers) + " worker(s)");
    }
}

} // namespace

int main() {
    test_player_shot();
    test_rejections();
    test_autofire();
    test_replay();
    test_workers();
    if (failures != 0) {
        std::cerr << failures << " ion shot contract test(s) failed\n";
        return 1;
    }
    std::cout << "ion shot contracts passed\n";
    return 0;
}
