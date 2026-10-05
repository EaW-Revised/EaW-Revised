#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/ion.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #561: ion weapons (docs/behaviour/space-damage.md EN-07, IS-01 to IS-09). The pure rules are
// checked on small values; the session contracts cover the energy drain, the stun's fire-rate and
// speed cuts and its expiry, with identical hashes at 1, 2, 4 and 8 workers.
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

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] Fixed decimal(const std::string_view text) { return Fixed::from_decimal(text).value(); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}

// FoC's shield and energy constants (see damage_tests.cpp). Damage types: 0 an ion type, 1 a
// laser; armor types: 0 Armor_Frigate, 1 Shield_Frigate. The ion type does x2 against the shield
// armor (Damage_IonCannon_LRG against a corvette's shield in FoC).
[[nodiscard]] tactical::DamageRules rules() {
    tactical::DamageRules result;
    result.shield_recharge_frames = 90;
    result.depleted_disable_seconds = units(5);
    result.depleted_increment_seconds = Fixed{};
    result.depleted_regen_cap = decimal("0.25");
    result.diminishing = {{units(0), units(1)}, {units(2), units(1)}};
    result.damage_types = 2;
    result.armor_types = 2;
    result.armor_mods = {units(1), units(2), units(1), units(1)};
    result.energy_recharge_frames = 150;
    result.energy_to_shield = units(5);
    return result;
}

constexpr tactical::TypeId shooter_type = 1;
constexpr tactical::TypeId frigate_type = 2; // shielded, powered, ion-stunnable, armed
constexpr tactical::TypeId corvette_type = 3; // unshielded, powered, ion-stunnable, moves

[[nodiscard]] tactical::DurabilityProfile frigate() {
    tactical::DurabilityProfile profile;
    profile.type_id = frigate_type;
    profile.max_hull = units(600);
    profile.max_shields = units(100);
    profile.shield_refresh = units(10);
    profile.armor_type = 0;
    profile.shield_armor_type = 1;
    profile.powered = true;
    profile.max_energy = units(300);
    profile.energy_refresh = units(20);
    profile.ion_stun_effect = true;
    return profile;
}

// An ion projectile hit: 15 of type 0, shield damage, energy damage, no hitpoint damage; DG-05's
// gate off so the amounts stay whole.
[[nodiscard]] tactical::Hit ion_hit(const std::int64_t amount = 15) {
    tactical::Hit hit{units(amount), 0, true, true, false, tactical::hull_target, false, true};
    hit.energy_damage = true;
    return hit;
}

// --- EN-07: the drain ----------------------------------------------------------------------------

void test_drain() {
    const auto value = rules();
    const auto profile = frigate();
    auto state = tactical::full_durability(profile);
    auto outcome = tactical::apply_hit(profile, value, state, ion_hit(), 10);
    expect(outcome && outcome.value().absorbed.raw() == 30 * one && outcome.value().drained.raw() == 0
            && state.shields.raw() == 70 * one && state.energy.raw() == 300 * one && state.hull.raw() == 600 * one,
        "EN-07: a shield that holds takes the ion hit x2 and nothing is drained");
    state.shields = units(10);
    outcome = tactical::apply_hit(profile, value, state, ion_hit(), 20);
    expect(outcome && outcome.value().absorbed.raw() == 10 * one && outcome.value().drained.raw() == 20 * one
            && state.energy.raw() == 280 * one && state.hull.raw() == 600 * one,
        "EN-07: what the shield leaves (20 of 30, still at x2) drains the pool; the hull is untouched");
    outcome = tactical::apply_hit(profile, value, state, ion_hit(), 30);
    expect(outcome && outcome.value().absorbed.raw() == 0 && outcome.value().drained.raw() == 30 * one
            && state.energy.raw() == 250 * one,
        "EN-07: during the depletion effect the whole ion hit drains the pool");
    state.energy = units(20);
    outcome = tactical::apply_hit(profile, value, state, ion_hit(), 40);
    expect(outcome && outcome.value().drained.raw() == 20 * one && state.energy.raw() == 0,
        "EN-07: the drain stops at an empty pool");
    auto unpowered = frigate();
    unpowered.powered = false;
    auto dry = tactical::full_durability(unpowered);
    dry.shields = Fixed{};
    outcome = tactical::apply_hit(unpowered, value, dry, ion_hit(), 10);
    expect(outcome && outcome.value().drained.raw() == 0 && dry.hull.raw() == 600 * one,
        "EN-07: a unit without a pool loses nothing to an ion hit on a bare shield");
    auto corvette = frigate();
    corvette.type_id = corvette_type;
    corvette.max_shields = Fixed{};
    corvette.shield_refresh = Fixed{};
    auto bare = tactical::full_durability(corvette);
    outcome = tactical::apply_hit(corvette, value, bare, ion_hit(), 10);
    expect(outcome && outcome.value().drained.raw() == 30 * one && bare.energy.raw() == 270 * one,
        "EN-07: an unshielded unit with a pool is drained at its shield armor (x2)");
    auto laser = ion_hit();
    laser.energy_damage = false;
    laser.hitpoint_damage = true;
    laser.damage_type = 1;
    auto plain = tactical::full_durability(corvette);
    outcome = tactical::apply_hit(corvette, value, plain, laser, 10);
    expect(outcome && outcome.value().drained.raw() == 0 && plain.energy.raw() == 300 * one && plain.hull.raw() == 585 * one,
        "EN-07: a projectile without energy damage drains nothing and hits the hull as before");
    auto scripted = ion_hit();
    scripted.projectile = false;
    auto held = tactical::full_durability(corvette);
    outcome = tactical::apply_hit(corvette, value, held, scripted, 10);
    expect(outcome && outcome.value().drained.raw() == 0 && held.energy.raw() == 300 * one,
        "EN-07: scripted damage never drains");
}

// --- IS-03 to IS-06: the stun ----------------------------------------------------------------------

void test_stun_rules() {
    const tactical::IonStunShot shot{30, decimal("0.5"), decimal("0.5"), true};
    expect(tactical::valid_ion_stun(shot), "IS-01: FoC's Y-wing shot is a valid stun");
    expect(!tactical::valid_ion_stun({0, Fixed{}, Fixed{}, false}) && !tactical::valid_ion_stun({30, units(2), Fixed{}, false}),
        "IS-01: a stun without frames or with a reduction above 1 is rejected");
    auto stun = tactical::ion_stun(std::nullopt, shot, 100);
    expect(stun.end_frame == 130, "IS-03: a first stun ends its frames after the hit");
    expect(tactical::ion_stunned(stun, 129) && !tactical::ion_stunned(stun, 130), "IS-03: stunned before the end frame only");
    auto held = stun;
    stun = tactical::ion_stun(held, shot, 110);
    expect(stun.end_frame == 160, "IS-04: a stacking hit adds its frames");
    held = stun;
    stun = tactical::ion_stun(held, shot, 160);
    expect(stun.end_frame == 190, "IS-04: a hit in the end frame still stacks");
    auto single = shot;
    single.stack = false;
    stun = tactical::ion_stun(held, single, 110);
    expect(stun.end_frame == 140, "IS-04: a non-stacking hit restarts the stun from its frame");
    expect(tactical::ion_speed_factor(stun, 120) == decimal("0.5") && tactical::ion_speed_factor(stun, 140) == units(1)
            && tactical::ion_fire_rate(stun, 120) == decimal("0.5") && tactical::ion_fire_rate(std::nullopt, 0) == units(1),
        "IS-05, IS-06: the speed and fire-rate factors while stunned and after");
    const auto half = decimal("0.5");
    expect(tactical::fire_rate_recharge(90, half) == 180 && tactical::fire_rate_recharge(7, half) == 14
            && tactical::fire_rate_recharge(10, decimal("0.3")) == 33 && tactical::fire_rate_recharge(90, units(1)) == 90,
        "IS-06: a recharge after a burst is divided by the fire rate, rounded half up");
    expect(tactical::fire_rate_pulses(5, half) == 3 && tactical::fire_rate_pulses(2, half) == 1
            && tactical::fire_rate_pulses(1, half) == 1 && tactical::fire_rate_pulses(5, units(1)) == 5,
        "IS-06: the next burst has the pulse count x the fire rate, rounded half up");
    expect(tactical::fire_rate_gap(6, half) == 12 && tactical::fire_rate_gap(6, Fixed{}) == 0,
        "IS-06: the gap within a burst is divided by the fire rate; zero at no fire rate");
    // AB-14: DEFEND is refused under ion stun.
    const tactical::AbilityProfile defend{tactical::AbilityKind::defend, 450, 1800, {}, true};
    tactical::AbilityGate gate;
    gate.shielded = true;
    gate.shields_online = true;
    const tactical::AbilitySlot slot;
    expect(tactical::ability_ready(defend, slot, gate, 0), "AB-14: DEFEND is ready on a sound shield");
    gate.ion_stunned = true;
    expect(!tactical::ability_ready(defend, slot, gate, 0), "AB-14: DEFEND is refused under ion stun");
}

// --- Sessions --------------------------------------------------------------------------------------

// A HP_Nebulon_Weapon-like laser (see damage_tests.cpp): 5 shots 6 frames apart, recharge 3 s.
[[nodiscard]] tactical::WeaponProfile laser(const std::uint32_t hardpoint = 0) {
    tactical::WeaponProfile weapon;
    weapon.hardpoint = hardpoint;
    weapon.range = units(700);
    weapon.min_recharge_hundredths = 300;
    weapon.max_recharge_hundredths = 300;
    weapon.pulse_count = 5;
    weapon.pulse_delay_frames = 6;
    weapon.cone_width = units(360);
    weapon.cone_height = units(180);
    weapon.opportunity_when_idle = true;
    weapon.opportunity_when_targeting = true;
    weapon.fire_a = at(20, 0);
    weapon.shot = tactical::ShotProfile{units(10), 1, units(25), units(700), true, true, {}};
    return weapon;
}

// The shooter's ion cannon: one shot every second of 15 ion damage that drains and, with `stun`,
// stuns for 30 frames at half speed and half fire rate, stacking (FoC's Y-wing shot's stun).
[[nodiscard]] tactical::WeaponProfile ion_cannon(const bool stun) {
    auto weapon = laser();
    weapon.pulse_count = 1;
    weapon.min_recharge_hundredths = 100;
    weapon.max_recharge_hundredths = 100;
    weapon.shot = tactical::ShotProfile{units(15), 0, units(25), units(700), true, false, {}};
    weapon.shot->energy_damage = true;
    if (stun) weapon.shot->ion_stun = tactical::IonStunShot{30, decimal("0.5"), decimal("0.5"), true};
    return weapon;
}

[[nodiscard]] tactical::CombatTable combat(const bool stun) {
    tactical::CombatTable table;
    const tactical::CollisionBox hull_box{at(-20, -10, -10), at(20, 10, 10)};
    tactical::CombatProfile shooter;
    shooter.type_id = shooter_type;
    shooter.category_bits = 1;
    shooter.max_attack_distance = units(700);
    shooter.weapons = {ion_cannon(stun)};
    shooter.collision = hull_box;
    tactical::CombatProfile ship;
    ship.type_id = frigate_type;
    ship.category_bits = 2;
    ship.max_attack_distance = units(700);
    ship.weapons = {laser()};
    ship.collision = hull_box;
    tactical::CombatProfile corvette = ship;
    corvette.type_id = corvette_type;
    corvette.weapons.clear();
    table.profiles = {shooter, ship, corvette};
    return table;
}

[[nodiscard]] tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
    table.damage = rules();
    tactical::DurabilityProfile shooter;
    shooter.type_id = shooter_type;
    shooter.max_hull = units(100000);
    shooter.armor_type = 0;
    auto corvette = frigate();
    corvette.type_id = corvette_type;
    corvette.max_shields = Fixed{};
    corvette.shield_refresh = Fixed{};
    table.profiles = {shooter, frigate(), corvette};
    return table;
}

// The corvette crawls at 3 units per frame.
[[nodiscard]] tactical::MotionTable motion() {
    tactical::MotionTable table;
    table.rules = {units(15), units(300)};
    tactical::MotionProfile profile;
    profile.type_id = corvette_type;
    profile.max_speed = units(3);
    profile.acceleration = units(3);
    profile.deceleration = units(3);
    profile.rate_of_turn = units(90);
    profile.turn_in_place_slowdown = units(1);
    table.profiles.push_back(profile);
    return table;
}

[[nodiscard]] std::vector<tactical::SensorProfile> sensors() {
    std::vector<tactical::SensorProfile> result;
    for (tactical::TypeId type = 1; type <= 3; ++type) result.push_back({type, units(3000)});
    return result;
}

[[nodiscard]] tactical::UnitState unit(const eawr::sim::EntityId id, const tactical::TypeId type,
    const tactical::PlayerId owner, const math::Vec3 position, const bool facing_west = false) {
    tactical::UnitState state;
    state.entity_id = id;
    state.type_id = type;
    state.owner = owner;
    state.position = position;
    state.rotation = facing_west ? math::Quat{Fixed{}, Fixed{}, units(1), Fixed{}} : math::identity_quat();
    return state;
}

// Shooter 1 (player 1) at the origin; frigate 2 (player 2) 300 units east, firing back.
[[nodiscard]] tactical::TacticalSession duel(const bool stun, const tactical::TypeId target = frigate_type) {
    tactical::TacticalSetup setup;
    setup.seed = 5610;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {unit(1, shooter_type, 1, at(0, 0)), unit(2, target, 2, at(300, 0), true)};
    auto created = tactical::TacticalSession::create(setup, sensors(), durability(), motion(), std::nullopt, combat(stun));
    expect(static_cast<bool>(created), "ion session is created");
    return std::move(created).value();
}

struct Run {
    std::size_t target_shots{};  // the frigate's shots
    std::size_t stunned_ticks{}; // ticks the snapshot shows the target stunned
    std::uint32_t longest{};     // the most frames left any snapshot showed
    std::vector<std::string> rows;
};

Run run(tactical::TacticalSession value, const eawr::sim::PartitionExecutor& executor, const std::uint64_t ticks) {
    Run result;
    for (std::uint64_t index = 0; index < ticks; ++index) {
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "ion step succeeds");
        if (!stepped) break;
        const auto& snapshot = *stepped.value().snapshot;
        for (const auto& event : snapshot.combat_events()) {
            result.target_shots += event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 2;
        }
        for (const auto& instance : snapshot.instances()) {
            if (instance.entity_id != 2 || instance.ion_stun_frames == 0) continue;
            ++result.stunned_ticks;
            result.longest = std::max(result.longest, instance.ion_stun_frames);
        }
        result.rows.push_back(std::to_string(stepped.value().completed_tick) + ',' + stepped.value().state_sha256 + ','
            + snapshot.sha256());
    }
    return result;
}

// IS-06: the stunned frigate fires fewer shots than the same frigate hit by drain-only shots;
// EN-07: its pool drains while its hull stays whole.
void test_session_stun() {
    const eawr::sim::InlineExecutor inline_executor;
    const auto stunned = run(duel(true), inline_executor, 300);
    const auto control = run(duel(false), inline_executor, 300);
    expect(stunned.stunned_ticks > 100 && stunned.longest <= 60 && control.stunned_ticks == 0,
        "IS-03, IS-04: the frigate is stunned between hits (" + std::to_string(stunned.stunned_ticks)
            + " ticks, at most " + std::to_string(stunned.longest) + " frames left)");
    expect(stunned.target_shots < control.target_shots && stunned.target_shots > 0,
        "IS-06: the stun slows the frigate's fire (" + std::to_string(stunned.target_shots) + " against "
            + std::to_string(control.target_shots) + " shots)");
    auto drained = duel(false);
    for (int tick = 0; tick < 300; ++tick) static_cast<void>(drained.step(inline_executor));
    const auto pool = drained.durability_state(2);
    expect(pool && pool->energy < units(300) && pool->hull.raw() == 600 * one,
        "EN-07: in a session the ion cannon drains the frigate's pool and leaves its hull");
    // Contracts at 1, 2, 4 and 8 workers: every tick's state and snapshot hash match the inline run.
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(run(duel(true), executor, 300).rows == stunned.rows,
            "ion duel hashes match with " + std::to_string(workers) + " worker(s)");
    }
}

// IS-05: a stun does not slow a move under way (the corvette keeps its 3 units a frame); a move
// ordered while it is stunned is planned at half speed.
struct Crawl {
    std::vector<Fixed> steps;           // the corvette's northward step per tick
    std::vector<std::uint32_t> stunned; // its stun frames left per tick
    std::optional<std::size_t> reordered; // the tick the second move was ordered at
};

Crawl crawl(const bool stun, const bool reorder = false) {
    const eawr::sim::InlineExecutor inline_executor;
    auto value = duel(stun, corvette_type);
    expect(static_cast<bool>(value.submit({{0, 2, 0}, {2}, tactical::MovePayload{at(300, 3000)}})), "the corvette moves");
    Crawl result;
    auto previous = at(300, 0);
    for (int tick = 0; tick < 200; ++tick) {
        auto stepped = value.step(inline_executor);
        expect(static_cast<bool>(stepped), "corvette step succeeds");
        if (!stepped) break;
        // A second move, ordered once the corvette has at least 20 stun frames left.
        if (reorder && !result.reordered && tick >= 30) {
            for (const auto& instance : stepped.value().snapshot->instances()) {
                if (instance.entity_id != 2 || instance.ion_stun_frames < 20) continue;
                const auto next = stepped.value().completed_tick + 1;
                expect(static_cast<bool>(value.submit({{next, 2, 1}, {2}, tactical::MovePayload{at(300, 3000)}})),
                    "the stunned corvette takes a new move");
                result.reordered = static_cast<std::size_t>(tick + 1);
            }
        }
        const auto live = value.units();
        const auto found = std::find_if(live.begin(), live.end(), [](const auto& entry) { return entry.entity_id == 2; });
        if (found == live.end()) break;
        result.steps.push_back(Fixed::from_raw(std::abs(found->position.y.raw() - previous.y.raw())));
        previous = found->position;
        std::uint32_t left = 0;
        for (const auto& instance : stepped.value().snapshot->instances()) {
            if (instance.entity_id == 2) left = instance.ion_stun_frames;
        }
        result.stunned.push_back(left);
    }
    return result;
}

[[nodiscard]] double as_double(const Fixed value) {
    return static_cast<double>(value.raw()) / static_cast<double>(one);
}

void test_session_speed() {
    const auto under_way = crawl(true);
    const auto free = crawl(false);
    Fixed fastest_free{};
    for (std::size_t tick = 10; tick < free.steps.size(); ++tick) fastest_free = std::max(fastest_free, free.steps[tick]);
    // A move under way keeps its speed through the stun (IS-05, G-D9).
    Fixed slowest_stunned = units(100);
    std::size_t counted = 0;
    for (std::size_t tick = 10; tick < under_way.steps.size(); ++tick) {
        if (under_way.stunned[tick] == 0) continue;
        ++counted;
        slowest_stunned = std::min(slowest_stunned, under_way.steps[tick]);
    }
    expect(counted > 20, "IS-05: the corvette is stunned while it moves (" + std::to_string(counted) + " ticks)");
    expect(fastest_free >= decimal("2.9") && slowest_stunned >= decimal("2.9"),
        "IS-05: a move under way keeps its 3 units a frame while stunned (" + std::to_string(as_double(slowest_stunned))
            + " against " + std::to_string(as_double(fastest_free)) + ")");
    // A move ordered while stunned is planned at half speed, after a move's usual latency.
    const auto reordered = crawl(true, true);
    expect(reordered.reordered.has_value(), "IS-05: the corvette is stunned when the second move is ordered");
    if (reordered.reordered) {
        Fixed fastest{};
        for (std::size_t tick = *reordered.reordered + 5; tick < std::min(reordered.steps.size(), *reordered.reordered + 40); ++tick) {
            fastest = std::max(fastest, reordered.steps[tick]);
        }
        expect(fastest <= decimal("1.51"),
            "IS-05: a move ordered while stunned goes at most half its 3 units a frame (" + std::to_string(as_double(fastest)) + ")");
    }
}

void test_engine_deadline() {
    auto profile = frigate();
    profile.max_speed = units(3);
    profile.hardpoints = {{tactical::HardpointRole::engine, true, units(10)}};
    auto state = tactical::full_durability(profile);
    tactical::disable_engines(state, 30, 10);
    expect(state.engines_disabled_until == 40U && !tactical::engines_online(profile, state), "EN-09: engines disabled to frame 40");
    tactical::disable_engines(state, 10, 20);
    expect(state.engines_disabled_until == 40U, "EN-09: shorter reapplication cannot shorten the deadline");
    tactical::disable_engines(state, 30, 25);
    expect(state.engines_disabled_until == 55U, "EN-09: repeated hits keep the later deadline, without stacking");
    expect(!tactical::service_disabled_engines(state, 54), "EN-09: engines stay offline before the deadline");
    state.hardpoints[0] = Fixed{};
    expect(tactical::service_disabled_engines(state, 55) && !state.engines_disabled_until
        && !tactical::engines_online(profile, state), "EN-09: timer expiry never repairs destroyed engines");
    state.hardpoints[0] = units(10);
    tactical::disable_engines(state, 0, 56);
    expect(tactical::service_disabled_engines(state, 56) && tactical::engines_online(profile, state),
        "EN-09: zero-duration disable recovers at its service frame");
}

struct EngineRun {
    bool disabled{};
    bool recovered{};
    Fixed slowest{units(100)};
    std::vector<std::string> rows;
};

EngineRun engine_run(const eawr::sim::PartitionExecutor& executor, const bool flag = true,
    const bool powered = true, const bool shield_holds = false, const std::uint32_t duration = 30) {
    auto health = durability();
    auto& target = health.profiles.back();
    target.max_speed = units(3);
    target.powered = powered;
    target.max_energy = powered ? units(300) : Fixed{};
    if (shield_holds) target.max_shields = units(100000);
    auto weapons = combat(false);
    if (flag) weapons.profiles.front().weapons.front().shot->disable_engines_frames = duration;
    tactical::TacticalSetup setup;
    setup.seed = 5610;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(300, 0))};
    auto created = tactical::TacticalSession::create(setup, sensors(), health, motion(), std::nullopt, weapons);
    expect(static_cast<bool>(created), "EN-08: engine-disable session creates");
    if (!created) return {};
    auto value = std::move(created).value();
    expect(static_cast<bool>(value.submit({{0, 2, 0}, {2}, tactical::MovePayload{at(300, 3000)}})), "EN-08: target cruises");
    EngineRun result;
    auto previous = at(300, 0);
    std::optional<std::uint64_t> last_deadline;
    for (std::uint64_t tick = 0; tick < 200; ++tick) {
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "EN-08: session step succeeds");
        if (!stepped) break;
        const auto state = value.durability_state(2);
        expect(state && state->hull == target.max_hull, "EN-08: ordinary ion leaves target hull whole");
        if (state && state->engines_disabled_until) {
            last_deadline = state->engines_disabled_until;
            if (!result.disabled) {
                result.disabled = true;
                expect(static_cast<bool>(value.submit({{stepped.value().completed_tick, 1, 0}, {1},
                    tactical::DamagePayload{units(100000), tactical::hull_target}})), "EN-08: remove shooter after first drain");
            }
        }
        for (const auto& instance : stepped.value().snapshot->instances()) {
            if (instance.entity_id != 2 || !instance.durability) continue;
            if (state && state->engines_disabled_until) {
                expect(!instance.durability->engines_online, "EN-08: snapshot publishes engines offline");
                expect(instance.durability->max_speed_factor == decimal("0.4"), "EN-08: snapshot publishes disabled speed factor");
                result.slowest = std::min(result.slowest,
                    Fixed::from_raw(std::abs(instance.fixed_transform.rows[1][3].raw() - previous.y.raw())));
            } else if (result.disabled) {
                result.recovered = true;
                expect(tick >= *last_deadline && instance.durability->engines_online,
                    "EN-09: engines recover at or after the last hit's deadline");
            }
        }
        const auto live = value.units();
        const auto found = std::find_if(live.begin(), live.end(), [](const auto& entry) { return entry.entity_id == 2; });
        if (found != live.end()) previous = found->position;
        result.rows.push_back(stepped.value().state_sha256 + ',' + stepped.value().snapshot->sha256());
    }
    return result;
}

void test_session_engine_disable() {
    const eawr::sim::InlineExecutor executor;
    const auto reference = engine_run(executor);
    expect(reference.disabled && reference.recovered, "EN-08/09: a draining small ion disables engines then recovers");
    expect(reference.slowest < units(2), "EN-09: engine disable slows a move already under way");
    expect(!engine_run(executor, false).disabled, "EN-08: an ordinary ion without the disable flag leaves engines online");
    expect(!engine_run(executor, true, false).disabled, "EN-08: a target without an energy pool cannot trigger disable");
    expect(!engine_run(executor, true, true, true).disabled, "EN-08: a fully absorbed hit cannot trigger disable");
    expect(engine_run(executor, true, true, false, 60).rows != reference.rows,
        "EN-09: disable duration changes authoritative state hashes");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter parallel(workers);
        expect(engine_run(parallel).rows == reference.rows, "EN-08: engine-disable hashes match at " + std::to_string(workers) + " workers");
    }
}

void test_validation() {
    auto table = combat(true);
    expect(static_cast<bool>(tactical::validate_combat(table)), "the ion combat table is valid");
    table.profiles[0].weapons[0].shot->ion_stun->frames = 0;
    expect(!tactical::validate_combat(table), "a stun without frames is rejected");
}

} // namespace

int main() {
    test_validation();
    test_drain();
    test_stun_rules();
    test_session_stun();
    test_session_speed();
    test_engine_deadline();
    test_session_engine_disable();
    if (failures != 0) {
        std::cerr << failures << " ion contract test(s) failed\n";
        return 1;
    }
    std::cout << "ion contracts passed\n";
    return 0;
}
