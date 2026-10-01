#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// P2-11 (#74): projectiles, damage types against armor, shield absorption and regeneration
// (docs/behaviour/space-damage.md). The pure rules are checked against FoC values; the session
// fixtures cover shield loss, a hit destroying a hardpoint and an out-of-range miss; a battle's
// hashes are pinned in tactical-damage.hashes.csv and must be reproduced by every worker count,
// a scrambled storage order and a written-and-parsed replay. `damage_tests <fixtures> --update`
// rewrites the pin.
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
[[nodiscard]] bool close_to(const Fixed value, const double expected, const double tolerance) {
    return std::abs(static_cast<double>(value.raw()) / static_cast<double>(one) - expected) <= tolerance;
}

// --- Rules -------------------------------------------------------------------------------------

// FoC's gameconstants.xml: ShieldRechargeIntervalInSecs 3.0, Depleted_Shield_Disable_Time 5.0,
// Depleted_Shield_Damage_Increment 0, Depleted_Shield_Regen_Cap 0.25, Diminishing_Firepower
// 0, 0.6, 0.3, 0.7, 0.9, 0.9, 1, 1, 2, 1, EnergyRechargeIntervalInSecs 5.0 (150 frames),
// EnergyToShieldExchangeRate 5.0. Damage types: 0 Damage_Turbolaser, 1 Damage_Laser; armor
// types: 0 Armor_Frigate, 1 Shield_Frigate (Turbolaser: 3 and 2).
[[nodiscard]] tactical::DamageRules rules() {
    tactical::DamageRules result;
    result.shield_recharge_frames = 90;
    result.depleted_disable_seconds = units(5);
    result.depleted_increment_seconds = Fixed{};
    result.depleted_regen_cap = decimal("0.25");
    result.diminishing = {{units(0), decimal("0.6")}, {decimal("0.3"), decimal("0.7")}, {decimal("0.9"), decimal("0.9")},
        {units(1), units(1)}, {units(2), units(1)}};
    result.damage_types = 2;
    result.armor_types = 2;
    result.armor_mods = {units(3), units(2), units(1), units(1)};
    result.energy_recharge_frames = 150;
    result.energy_to_shield = units(5);
    return result;
}

constexpr tactical::TypeId shooter_type = 1;
constexpr tactical::TypeId frigate_type = 2; // shielded, hull 600, one destroyable weapon hardpoint
constexpr tactical::TypeId corvette_type = 3; // no shield, hull 300, one destroyable hardpoint
constexpr tactical::TypeId station_type = 4; // shielded, one shield generator

[[nodiscard]] tactical::DurabilityProfile frigate() {
    tactical::DurabilityProfile profile;
    profile.type_id = frigate_type;
    profile.max_hull = units(600);
    profile.hardpoints = {{tactical::HardpointRole::weapon, true, units(90), {}, {}}};
    profile.max_shields = units(100);
    profile.shield_refresh = units(10);
    profile.armor_type = 0;
    profile.shield_armor_type = 1;
    return profile;
}

void test_curve() {
    const auto value = rules();
    // S-15 hit sizes: 15 x f(8 frames) = 10.3833, 10 x f(1) = 6.1307, 10 x f(4) = 6.5082,
    // 10 x f(5) = 6.6242, 15 x f(2) = 9.39 (retail binary32).
    const std::vector<std::pair<std::uint64_t, double>> points{
        {0, 0.6}, {1, 0.61307}, {2, 0.626}, {4, 0.65082}, {5, 0.66242}, {8, 0.69222}, {9, 0.7}, {27, 0.9},
        {30, 1.0}, {36, 1.13871}, {45, 1.18061}, {60, 1.0}, {999999, 1.0}};
    for (const auto& [frames, expected] : points) {
        const auto factor = tactical::diminishing_factor(value, frames);
        expect(factor && close_to(factor.value(), expected, 0.0002),
            "diminishing factor at " + std::to_string(frames) + " frames is " + std::to_string(expected));
    }
    tactical::DamageRules flat;
    expect(tactical::diminishing_factor(flat, 3).value().raw() == one, "no curve: factor 1");
    expect(tactical::armor_multiplier(value, 0, 1).raw() == 2 * one, "Turbolaser vs Shield_Frigate is 2");
    expect(tactical::armor_multiplier(value, tactical::no_type_index, 1).raw() == one, "an unknown type multiplies by 1");
}

void test_hit_pipeline() {
    const auto profile = frigate();
    const auto value = rules();
    auto state = tactical::full_durability(profile);
    expect(state.shields.raw() == 100 * one, "a unit starts at full shield");
    // A first projectile hit (factor 1): 15 x 2 (shield armor) = 30 on a 100 shield.
    tactical::Hit hit{units(15), 0, true, true, true, tactical::hull_target};
    auto outcome = tactical::apply_hit(profile, value, state, hit, 100);
    expect(outcome && outcome.value().absorbed.raw() == 30 * one && state.shields.raw() == 70 * one
            && state.hull.raw() == 600 * one,
        "DG-06: the shield absorbs the damage times its armor multiplier; the hull is untouched");
    expect(outcome && outcome.value().shield_absorbed && outcome.value().armor_multiplier.raw() == one,
        "battle-presentation BP-12, BP-13: a hit the shield takes whole is absorbed and meets no armor");
    // Eight frames later: 15 x 0.69222 x 2 = 20.77 on the shield.
    outcome = tactical::apply_hit(profile, value, state, hit, 108);
    expect(outcome && close_to(state.shields, 70 - 15 * 0.69222 * 2, 0.001), "DG-05: the second hit shrinks with the gap");
    // A large hit empties the shield; the rest goes to the hull through both armor multipliers.
    state.shields = units(10);
    state.last_hit_frame.reset();
    hit.amount = units(20);
    outcome = tactical::apply_hit(profile, value, state, hit, 200);
    // 20 x 2 = 40 on the shield, 10 absorbed, (40 - 10) / 2 = 15 left, x 3 hull armor = 45.
    expect(outcome && outcome.value().shields_depleted && state.shields.raw() == 0 && state.hull.raw() == 555 * one
            && state.depleted_frame == std::optional<std::uint64_t>(200),
        "DG-07, DG-10: the remainder is scaled back and hits the hull by its armor; the shield is depleted");
    expect(outcome && !outcome.value().shield_absorbed && outcome.value().armor_multiplier.raw() == 3 * one,
        "battle-presentation BP-12, BP-13: a hit through the shield is not absorbed and carries the hull armor");
    // During the depletion effect the shield absorbs nothing.
    state.last_hit_frame.reset();
    hit.amount = units(10);
    outcome = tactical::apply_hit(profile, value, state, hit, 250);
    expect(outcome && state.shields.raw() == 0 && state.hull.raw() == 525 * one, "DG-09: a depleted shield absorbs nothing");
    expect(outcome && !outcome.value().shield_absorbed, "battle-presentation BP-12: a depleted shield absorbs no hit");
    // A projectile without shield damage passes the shield; one without hitpoint damage only
    // strips the shield.
    state.shields = units(50);
    state.depleted_frame.reset();
    state.last_hit_frame.reset();
    tactical::Hit piercing{units(10), 0, true, false, true, tactical::hull_target};
    outcome = tactical::apply_hit(profile, value, state, piercing, 400);
    expect(outcome && state.shields.raw() == 50 * one && state.hull.raw() == 495 * one,
        "DG-06: Projectile_Does_Shield_Damage no passes the shield");
    state.last_hit_frame.reset();
    tactical::Hit stripping{units(10), 0, true, true, false, tactical::hull_target};
    outcome = tactical::apply_hit(profile, value, state, stripping, 500);
    expect(outcome && state.shields.raw() == 30 * one && state.hull.raw() == 495 * one,
        "DG-10: Projectile_Does_Hitpoint_Damage no leaves the hull");
    // Scripted damage: no armor, no curve, the shield first (S-15 tick 500: 400 on a hardpoint
    // took 400 shield and left the hardpoint).
    state.shields = units(100);
    tactical::Hit scripted{units(40), tactical::no_type_index, false, true, true, 0};
    outcome = tactical::apply_hit(profile, value, state, scripted, 600);
    expect(outcome && state.shields.raw() == 60 * one && state.hardpoints[0].raw() == 90 * one,
        "DG-20: scripted damage is absorbed by the shield, with no multiplier");
    scripted.amount = units(160);
    outcome = tactical::apply_hit(profile, value, state, scripted, 601);
    expect(outcome && state.shields.raw() == 0 && state.hardpoints[0].raw() == 0
            && outcome.value().damage.destroyed_hardpoint == std::optional<std::uint32_t>(0),
        "DG-11: what the shield leaves goes to the named hardpoint");
}

void test_diminishing_gates() {
    // DG-05's two gates (#440): the shooter's allow-diminishing-firepower flag and the projectile's
    // internal damage type being the misc type. The curve applies, and the target's last-hit frame
    // advances, only when both hold; FoC's defaults (both on) keep every M2 hit unchanged.
    expect(tactical::Hit{}.allow_diminishing_firepower && tactical::Hit{}.internal_damage_misc,
        "DG-05: a default Hit keeps both gates on, matching FoC");
    expect(tactical::Projectile{}.allow_diminishing_firepower && tactical::Projectile{}.internal_damage_misc,
        "DG-05: a default Projectile keeps both gates on, matching FoC");

    tactical::DurabilityProfile profile;
    profile.type_id = 5;
    profile.max_hull = units(1000);
    const auto value = rules();

    // Both gates on: a never-hit-before unit takes factor 1 and starts the timer; a same-frame
    // second hit gets the curve's 0.6 (DC-01).
    {
        auto state = tactical::full_durability(profile);
        const tactical::Hit gated{units(10), 0, true, true, true, tactical::hull_target, true, true};
        auto first = tactical::apply_hit(profile, value, state, gated, 100);
        expect(first && state.hull.raw() == 990 * one && state.last_hit_frame == std::optional<std::uint64_t>(100),
            "DG-05: both gates on, a never-hit-before unit takes factor 1 and starts the timer");
        auto second = tactical::apply_hit(profile, value, state, gated, 100);
        expect(second && close_to(state.hull, 990.0 - 10 * 0.6, 0.001),
            "DG-05: both gates on, a same-frame hit gets the curve's 0.6");
    }

    // Gate (a) off: the shooter's flag is off. Every hit is full damage and the timer never starts.
    {
        auto state = tactical::full_durability(profile);
        const tactical::Hit no_flag{units(10), 0, true, true, true, tactical::hull_target, false, true};
        auto outcome = tactical::apply_hit(profile, value, state, no_flag, 100);
        expect(outcome && state.hull.raw() == 990 * one && !state.last_hit_frame,
            "DG-05: the shooter's flag off skips the curve and the timer");
        outcome = tactical::apply_hit(profile, value, state, no_flag, 100);
        expect(outcome && state.hull.raw() == 980 * one && !state.last_hit_frame,
            "DG-05: the flag off, a repeated same-frame hit is still full damage");
    }

    // Gate (b) off: a non-misc internal type behaves like the flag-off case.
    {
        auto state = tactical::full_durability(profile);
        const tactical::Hit not_misc{units(10), 0, true, true, true, tactical::hull_target, true, false};
        auto outcome = tactical::apply_hit(profile, value, state, not_misc, 100);
        expect(outcome && state.hull.raw() == 990 * one && !state.last_hit_frame,
            "DG-05: a non-misc internal damage type skips the curve and the timer");
    }

    // The timer advances only inside the gated branch: a skipped hit does not become the "previous
    // hit" for a later gated one, which still sees a never-hit-before unit (factor 1).
    {
        auto state = tactical::full_durability(profile);
        const tactical::Hit no_flag{units(10), 0, true, true, true, tactical::hull_target, false, true};
        const tactical::Hit gated{units(10), 0, true, true, true, tactical::hull_target, true, true};
        auto skipped = tactical::apply_hit(profile, value, state, no_flag, 100);
        auto after = tactical::apply_hit(profile, value, state, gated, 101);
        expect(skipped && after && state.hull.raw() == 980 * one
                && state.last_hit_frame == std::optional<std::uint64_t>(101),
            "DG-05: an ungated hit does not move the eligible-hit timer for the next gated hit");
    }

    // A hit with a DG-05 gate off still gets DG-26's out-of-combat factor: the two are independent
    // steps in FoC (DG-01), not DG-26 nested under DG-05's gates.
    {
        auto state = tactical::full_durability(profile);
        const tactical::Hit no_flag_out_of_combat{
            units(10), 0, true, true, true, tactical::hull_target, false, true, units(-1)};
        auto outcome = tactical::apply_hit(profile, value, state, no_flag_out_of_combat, 100);
        expect(outcome && state.hull.raw() == 980 * one,
            "DG-26: a DG-05-gate-off hit still gets doubled by the out-of-combat factor");
    }
}

// DG-26 (#409): a craft out of combat (defense -1) takes twice the damage; in combat, 1 times.
void test_out_of_combat() {
    const auto profile = frigate();
    const auto value = rules();
    auto state = tactical::full_durability(profile);
    state.shields = Fixed{};
    tactical::Hit hit{units(10), 0, true, false, true, tactical::hull_target, true, true, units(-1)};
    auto outcome = tactical::apply_hit(profile, value, state, hit, 100);
    // 10 x 2 (out of combat) x 3 (hull armor) = 60.
    expect(outcome && state.hull.raw() == 540 * one, "DG-26: an out-of-combat hit does twice its damage");
    state.last_hit_frame.reset();
    hit.defense = Fixed{};
    outcome = tactical::apply_hit(profile, value, state, hit, 200);
    expect(outcome && state.hull.raw() == 510 * one, "DG-26: an in-combat hit does its damage");
    state.last_hit_frame.reset();
    tactical::Hit scripted{
        units(10), tactical::no_type_index, false, true, true, tactical::hull_target, true, true, units(-1)};
    outcome = tactical::apply_hit(profile, value, state, scripted, 300);
    expect(outcome && state.hull.raw() == 500 * one, "DG-26: scripted damage takes no combat modifier");
}

// DG-26 with #530 PU-38: an arriving unit's -3 adds to the out-of-combat -1. FoC applies the modifier
// unclamped; the remake bounds it at -4..1, so -3 does 4 times, -4 five times and anything lower
// five times too.
void test_arrival_vulnerability() {
    const auto profile = frigate();
    const auto value = rules();
    auto state = tactical::full_durability(profile);
    state.shields = Fixed{};
    // 10 x 3 (hull armor) = 30 without a modifier.
    const auto hit_with = [&](const std::int64_t defense, const std::uint64_t frame) {
        state.last_hit_frame.reset();
        const auto before = state.hull;
        const tactical::Hit hit{units(10), 0, true, false, true, tactical::hull_target, true, true, units(defense)};
        const auto outcome = tactical::apply_hit(profile, value, state, hit, frame);
        expect(static_cast<bool>(outcome), "the hit applies");
        return (before.raw() - state.hull.raw()) / one;
    };
    expect(hit_with(-3, 100) == 120, "PU-38: an arriving unit takes 4 times the damage");
    expect(hit_with(-4, 200) == 150, "PU-38 and DG-26: an arriving craft out of combat takes 5 times");
    expect(hit_with(-6, 300) == 150, "DG-26: the remake bounds the modifier at -4");
}

void test_recharge() {
    const auto profile = frigate();
    const auto value = rules();
    auto state = tactical::full_durability(profile);
    state.shields = units(40);
    expect(static_cast<bool>(tactical::recharge_shields(profile, value, state, 10)) && state.shields.raw() == 50 * one,
        "DG-13: a recharge adds Shield_Refresh_Rate");
    state.shields = units(95);
    static_cast<void>(tactical::recharge_shields(profile, value, state, 11));
    expect(state.shields.raw() == 100 * one, "DG-13: capped at the maximum");
    // Depleted at frame 100: recharges stop at the cap until 150 frames have passed.
    state.shields = Fixed{};
    state.depleted_frame = 100;
    static_cast<void>(tactical::recharge_shields(profile, value, state, 180));
    expect(state.shields == decimal("0.25") && state.depleted_frame == std::optional<std::uint64_t>(100),
        "DG-15: during the depletion effect the shield recharges only to the cap (S-15: 0.25)");
    expect(tactical::shield_depleted(value, state, 249) && !tactical::shield_depleted(value, state, 250),
        "DG-08: the effect lasts Depleted_Shield_Disable_Time (150 frames)");
    static_cast<void>(tactical::recharge_shields(profile, value, state, 250));
    expect(state.shields == math::add(decimal("0.25"), units(10)).value() && !state.depleted_frame,
        "DG-15: after it, a recharge clears the effect");

    // A station with a shield generator: half the generators give half the refresh; the last lost
    // drops the shield and stops recharging.
    auto station = frigate();
    station.type_id = station_type;
    station.hardpoints = {{tactical::HardpointRole::shield_generator, true, units(50), {}, {}},
        {tactical::HardpointRole::shield_generator, true, units(50), {}, {}}};
    auto held = tactical::full_durability(station);
    held.shields = units(40);
    held.hardpoints[0] = Fixed{};
    static_cast<void>(tactical::recharge_shields(station, value, held, 10));
    expect(held.shields.raw() == 45 * one, "DG-14: the refresh scales with the generators left");
    held.hardpoints[1] = Fixed{};
    tactical::shield_generators_lost(station, value, held, 20);
    static_cast<void>(tactical::recharge_shields(station, value, held, 30));
    expect(held.shields.raw() == 0, "DG-17: without generators the shield drops and never recharges");
}

// EN-01 to EN-06 on a POWERED frigate: Energy_Capacity 300, Energy_Refresh_Rate 100.
void test_energy() {
    auto profile = frigate();
    profile.powered = true;
    profile.max_energy = units(300);
    profile.energy_refresh = units(100);
    const auto value = rules();
    auto state = tactical::full_durability(profile);
    expect(state.energy.raw() == 300 * one, "EN-01: the pool starts full");
    expect(tactical::has_energy_pool(profile, value) && !tactical::has_energy_pool(frigate(), value),
        "EN-01: only a POWERED type has a pool");
    state.energy = units(250);
    expect(static_cast<bool>(tactical::recharge_energy(profile, value, state)) && state.energy.raw() == 300 * one,
        "EN-02: a recharge adds Energy_Refresh_Rate up to Energy_Capacity");
    state.energy = units(60);
    state.shields = units(40);
    static_cast<void>(tactical::recharge_shields(profile, value, state, 10));
    expect(state.shields.raw() == 50 * one && state.energy.raw() == 10 * one,
        "EN-04: a shield recharge of 10 costs 10 x EnergyToShieldExchangeRate (50)");
    static_cast<void>(tactical::recharge_shields(profile, value, state, 100));
    expect(state.shields.raw() == 50 * one && state.energy.raw() == 10 * one,
        "EN-04: a pool short of the cost pays nothing and the shield gains nothing");
    state.shields = units(98);
    state.energy = units(10);
    static_cast<void>(tactical::recharge_shields(profile, value, state, 190));
    expect(state.shields.raw() == 100 * one && state.energy.raw() == 0,
        "EN-04: the cost follows the capped gain (2 points cost 10, all of the pool)");
    auto unpowered = tactical::full_durability(frigate());
    unpowered.shields = units(40);
    static_cast<void>(tactical::recharge_shields(frigate(), value, unpowered, 10));
    expect(unpowered.shields.raw() == 50 * one && unpowered.energy.raw() == 0,
        "EN-04: a type without a pool recharges free");
    state.energy = units(10);
    expect(tactical::draw_energy(profile, value, state, units(10)) && state.energy.raw() == 0,
        "EN-05: a shot may take the whole pool");
    expect(!tactical::draw_energy(profile, value, state, units(5)) && state.energy.raw() == 0,
        "EN-05: a pool short of the cost fires nothing");
    expect(tactical::draw_energy(profile, value, state, Fixed{}), "EN-05: a shot without a cost always fires");
    expect(!tactical::draw_energy(frigate(), value, unpowered, units(5)),
        "EN-05: a type without a pool never fires a shot that costs energy");
    auto off = value;
    off.energy_recharge_frames = 0;
    expect(!tactical::has_energy_pool(profile, off), "rules without an energy interval bind no pool");
}

void test_segment() {
    const tactical::CollisionBox box{at(-10, -5, -5), at(10, 5, 5)};
    const auto identity = math::to_matrix(math::identity_quat(), at(100, 0)).value();
    auto entry = tactical::segment_enters_box(box, identity, at(80, 0), at(105, 0));
    expect(entry && entry.value() && close_to(*entry.value(), 10.0 / 25.0, 1e-6), "a segment enters the box at its face");
    entry = tactical::segment_enters_box(box, identity, at(60, 0), at(85, 0));
    expect(entry && !entry.value(), "a segment that stops short misses");
    entry = tactical::segment_enters_box(box, identity, at(80, 8), at(105, 8));
    expect(entry && !entry.value(), "a segment beside the box misses");
    entry = tactical::segment_enters_box(box, identity, at(100, 0), at(125, 0));
    expect(entry && entry.value() && entry.value()->raw() == 0, "a segment starting inside enters at 0");
    // Turned a quarter: the box's long axis now lies along Y.
    const math::Quat quarter{Fixed{}, Fixed{}, decimal("0.70710678"), decimal("0.70710678")};
    const auto turned = math::to_matrix(math::normalize(quarter).value(), at(100, 0)).value();
    entry = tactical::segment_enters_box(box, turned, at(100, -30), at(100, -5));
    expect(entry && entry.value() && close_to(*entry.value(), 20.0 / 25.0, 1e-4), "the box turns with its unit");
}

// --- Sessions -----------------------------------------------------------------------------------

// A HP_Nebulon_Weapon-like laser: range 700, 175 x 160 cone, 5 shots 6 frames apart, recharge 3 to
// 4 s; its projectile: 10 damage of type 0, 25 units per frame.
[[nodiscard]] tactical::WeaponProfile laser(const std::int64_t travel = 700, const std::uint32_t hardpoint = 0) {
    tactical::WeaponProfile weapon;
    weapon.hardpoint = hardpoint;
    weapon.range = units(700);
    weapon.min_recharge_hundredths = 300;
    weapon.max_recharge_hundredths = 400;
    weapon.pulse_count = 5;
    weapon.pulse_delay_frames = 6;
    weapon.cone_width = units(175);
    weapon.cone_height = units(160);
    weapon.opportunity_when_idle = true;
    weapon.opportunity_when_targeting = true;
    weapon.fire_a = at(20, 0);
    weapon.shot = tactical::ShotProfile{units(10), 0, units(25), units(travel), true, true, {}};
    return weapon;
}

[[nodiscard]] tactical::CombatTable combat(const std::int64_t travel = 700) {
    tactical::CombatTable table;
    const tactical::CollisionBox hull_box{at(-20, -10, -10), at(20, 10, 10)};
    tactical::CombatProfile shooter;
    shooter.type_id = shooter_type;
    shooter.category_bits = 1;
    shooter.max_attack_distance = units(700);
    shooter.weapons = {laser(travel)};
    shooter.collision = hull_box;
    tactical::CombatProfile ship;
    ship.type_id = frigate_type;
    ship.category_bits = 2;
    ship.hardpoints = {{0, at(15, 0), true}};
    ship.collision = hull_box;
    tactical::CombatProfile corvette = ship;
    corvette.type_id = corvette_type;
    tactical::CombatProfile station = ship;
    station.type_id = station_type;
    station.hardpoints.clear();
    table.profiles = {shooter, ship, corvette, station};
    return table;
}

[[nodiscard]] tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
    table.damage = rules();
    tactical::DurabilityProfile shooter;
    shooter.type_id = shooter_type;
    shooter.max_hull = units(2000);
    shooter.armor_type = 0;
    auto corvette = frigate();
    corvette.type_id = corvette_type;
    corvette.max_hull = units(300);
    corvette.max_shields = Fixed{};
    corvette.shield_refresh = Fixed{};
    auto station = frigate();
    station.type_id = station_type;
    station.hardpoints.clear();
    // EN-04: the frigate pays for its shield from a pool that can run dry in the battle.
    auto ship = frigate();
    ship.powered = true;
    ship.max_energy = units(120);
    ship.energy_refresh = units(20);
    table.profiles = {shooter, ship, corvette, station};
    return table;
}

[[nodiscard]] std::vector<tactical::SensorProfile> sensors() {
    std::vector<tactical::SensorProfile> result;
    for (tactical::TypeId type = 1; type <= 4; ++type) result.push_back({type, units(3000)});
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

[[nodiscard]] tactical::TacticalSetup setup(std::vector<tactical::UnitState> list) {
    tactical::TacticalSetup result;
    result.seed = 7401;
    result.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    result.units = std::move(list);
    return result;
}

[[nodiscard]] tactical::TacticalSession session(const tactical::TacticalSetup& value, const std::int64_t travel = 700) {
    auto created = tactical::TacticalSession::create(value, sensors(), durability(), {}, std::nullopt, combat(travel));
    expect(static_cast<bool>(created), "damage session is created");
    return std::move(created).value();
}

struct Tally {
    std::size_t shots{};
    std::size_t hits{};
    std::size_t absorbed{}; // hits the shield took whole (hit_outcome_shield_absorbed, #80)
    std::vector<tactical::Event> events;
};

Tally run(tactical::TacticalSession& value, const std::uint64_t ticks) {
    Tally tally;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t index = 0; index < ticks; ++index) {
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "damage step succeeds");
        if (!stepped) break;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            tally.shots += event.kind == tactical::CombatEventKind::weapon_fired;
            tally.hits += event.kind == tactical::CombatEventKind::projectile_hit;
            tally.absorbed += event.kind == tactical::CombatEventKind::projectile_hit
                && (event.outcome & tactical::hit_outcome_shield_absorbed) != 0U;
        }
        const auto events = stepped.value().snapshot->events();
        tally.events.insert(tally.events.end(), events.begin(), events.end());
    }
    return tally;
}

// #530 PU-38, PU-39 in a battle: player 1 buys an armed unit and brings it in at (0, 0), 300 units
// from player 2's shooter. It holds its fire until its arrival ends (frame 150); the first hit it
// takes before then does what apply_hit gives for a defense modifier of -3.
void test_arrival_in_battle() {
    const auto staged = setup({unit(1, station_type, 1, at(-2000, 0)), unit(2, shooter_type, 2, at(300, 0), true)});
    tactical::EconomyRules economy;
    economy.players = {{1, units(1000), 25, false, Fixed{}}, {2, units(1000), 25, true, units(180)}};
    economy.menus = {{station_type, 1,
        {{shooter_type, tactical::BuildKind::unit, tactical::BuildQueue::units, units(100), 1, 1, 1, true}}}};
    economy.vulnerability = units(-3);
    economy.vulnerability_frames = 150;
    auto created = tactical::TacticalSession::create(staged, sensors(), durability(), {}, std::nullopt, combat(), {}, {}, economy);
    expect(static_cast<bool>(created), "the arrival battle session is created");
    if (!created) return;
    auto value = std::move(created).value();
    expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::BuyPayload{shooter_type}})), "player 1 buys the unit");
    static_cast<void>(run(value, 5));
    const auto id = value.next_entity_id();
    expect(static_cast<bool>(value.submit({{5, 1, 1}, {}, tactical::ReinforcePayload{shooter_type, at(0, 0)}})),
        "player 1 brings it in");
    constexpr std::uint64_t start = 5; // arrival frame 0 is tick 5, frame 150 tick 155
    std::optional<std::uint64_t> first_fired;
    std::optional<std::uint64_t> first_hit;
    Fixed hull_before_hit{};
    Fixed hull_after_hit{};
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t tick = start; tick < start + 400; ++tick) {
        const auto before = value.durability_state(id);
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "arrival battle step succeeds");
        if (!stepped) break;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            if (event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == id && !first_fired) first_fired = tick;
            if (event.kind == tactical::CombatEventKind::projectile_hit && event.target == id && !first_hit && before) {
                first_hit = tick;
                hull_before_hit = before->hull;
                hull_after_hit = value.durability_state(id)->hull;
            }
        }
    }
    expect(first_fired.has_value() && *first_fired >= start + 150,
        "PU-39: the arriving unit holds its fire until its arrival ends, then fires");
    expect(first_hit.has_value() && *first_hit >= start + 35 && *first_hit < start + 150,
        "PU-37, PU-38: the enemy hits it only once it is shown, and while it arrives");
    if (!first_hit) return;
    const auto profile = durability().profiles[0];
    auto expected = tactical::full_durability(profile);
    const tactical::Hit hit{units(10), 0, true, true, true, tactical::hull_target, true, true, units(-3)};
    const auto outcome = tactical::apply_hit(profile, *durability().damage, expected, hit, *first_hit);
    expect(outcome && hull_before_hit.raw() - hull_after_hit.raw() == profile.max_hull.raw() - expected.hull.raw(),
        "PU-38: the hit does what a -3 defense modifier gives (4 times)");
}

// Fixture DG-F1, shield loss: a shooter 300 units from a shielded frigate. Its projectiles fly 25
// units per frame, so each shot lands about 11 frames after it is fired; the shield absorbs every
// hit at the shield armor (x1 for type 0 vs armor 1 here: 2 in the table) until it is empty.
void test_shield_loss() {
    auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(300, 0), true)}));
    auto tally = run(value, 60);
    const auto health = value.durability_state(2);
    expect(tally.shots > 0 && tally.hits > 0 && tally.hits <= tally.shots, "the shooter fires and its projectiles hit");
    expect(health && health->shields.raw() < 100 * one && health->hull.raw() == 600 * one,
        "shield loss: the shield takes the hits, the hull none");
    expect(tally.absorbed == tally.hits, "#80: every hit the shield takes whole is marked absorbed");
    expect(value.snapshot()->instances()[1].durability->shields == health->shields,
        "the snapshot carries the shield");
    tally = run(value, 400);
    const auto later = value.durability_state(2);
    expect(!later || later->hull.raw() < 600 * one, "once the shield is gone the hull takes damage");
    expect(tally.absorbed < tally.hits, "#80: hits that reach the hull are not marked absorbed");
}

// Fixture DG-F2, hull damage destroying a hardpoint: an unshielded corvette whose targetable,
// destroyable hardpoint (90 health, armor x3 for type 0) is the aim point. Three 10-damage hits
// (x3, shrunk by the curve) destroy it; the hull stays whole (HD-02).
void test_hardpoint_destroyed() {
    auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(300, 0), true)}));
    const auto tally = run(value, 120);
    const auto health = value.durability_state(2);
    bool destroyed = false;
    for (const auto& event : tally.events) {
        destroyed = destroyed || (event.kind == tactical::EventKind::hardpoint_destroyed && event.unit == 2 && event.hardpoint == 0);
    }
    expect(destroyed && health && health->hardpoints[0].raw() == 0, "the aimed hardpoint is destroyed by projectile hits");
    expect(health && health->hull.raw() == 300 * one, "hits on a live hardpoint leave the hull");
}

// #536, DG-36: a square plate in the local YZ plane at x = `x`, half size `half`.
[[nodiscard]] std::vector<tactical::CollisionTriangle> plate(const std::int64_t x, const std::int64_t half) {
    return {{at(x, -half, -half), at(x, half, -half), at(x, half, half)},
        {at(x, -half, -half), at(x, half, half), at(x, -half, half)}};
}

// Brute force: every triangle of every enabled mesh on its own, for the tree's check.
[[nodiscard]] std::optional<tactical::MeshHit> brute(const std::vector<tactical::CollisionMesh>& meshes,
    const math::Mat3x4& transform, const math::Vec3& from, const math::Vec3& to) {
    std::optional<tactical::MeshHit> best;
    for (std::size_t index = 0; index < meshes.size(); ++index) {
        for (const auto& triangle : meshes[index].triangles) {
            const std::vector<tactical::CollisionMesh> single{
                tactical::collision_mesh({triangle}, tactical::no_hardpoint, tactical::no_hardpoint, false)};
            const auto hit = tactical::segment_hits_meshes(single, [](std::size_t) { return true; }, transform, from, to);
            if (hit && hit.value() && (!best || hit.value()->fraction < best->fraction)) {
                best = tactical::MeshHit{hit.value()->fraction, index};
            }
        }
    }
    return best;
}

void test_meshes() {
    const auto all = [](std::size_t) { return true; };
    const auto placed = math::to_matrix(math::identity_quat(), at(100, 0)).value();
    std::vector<tactical::CollisionMesh> meshes{
        tactical::collision_mesh(plate(0, 5), tactical::no_hardpoint, tactical::no_hardpoint, false)};
    auto hit = tactical::segment_hits_meshes(meshes, all, placed, at(80, 0), at(105, 0));
    expect(hit && hit.value() && hit.value()->fraction.raw() == one * 4 / 5 && hit.value()->mesh == 0,
        "DG-36: a step meets the plate at its fraction");
    hit = tactical::segment_hits_meshes(meshes, all, placed, at(80, 6), at(105, 6));
    expect(hit && !hit.value(), "DG-36: a step beside the plate misses");
    hit = tactical::segment_hits_meshes(meshes, all, placed, at(60, 0), at(85, 0));
    expect(hit && !hit.value(), "DG-36: a step that stops short misses");
    hit = tactical::segment_hits_meshes(meshes, all, placed, at(100, -20, 1), at(100, 20, 1));
    expect(hit && !hit.value(), "DG-36: a step in the plate's plane is parallel and misses");
    hit = tactical::segment_hits_meshes(meshes, all, placed, at(120, 0), at(95, 0));
    expect(hit && hit.value() && hit.value()->fraction.raw() == one * 4 / 5, "DG-36: both faces of a triangle count");
    // A hardpoint plate in front of the hull plate wins; switched off, the hull plate is met.
    meshes.push_back(tactical::collision_mesh(plate(-5, 2), 0, 0, false));
    hit = tactical::segment_hits_meshes(meshes, all, placed, at(80, 0), at(105, 0));
    expect(hit && hit.value() && hit.value()->mesh == 1 && hit.value()->fraction.raw() == one * 3 / 5,
        "DG-36: the first mesh along the step wins");
    hit = tactical::segment_hits_meshes(
        meshes, [](std::size_t index) { return index != 1; }, placed, at(80, 0), at(105, 0));
    expect(hit && hit.value() && hit.value()->mesh == 0, "DG-38: a switched-off mesh lets the step through");
    // The mesh turns with its unit: a quarter turn puts the plate across Y.
    const math::Quat quarter{Fixed{}, Fixed{}, decimal("0.70710678"), decimal("0.70710678")};
    const auto turned = math::to_matrix(math::normalize(quarter).value(), at(100, 0)).value();
    hit = tactical::segment_hits_meshes(
        meshes, [](std::size_t index) { return index == 0; }, turned, at(100, -20), at(100, 5));
    expect(hit && hit.value() && close_to(hit.value()->fraction, 20.0 / 25.0, 1e-4), "DG-36: the mesh turns with its unit");

    // The tree finds what every triangle on its own finds: a 20 x 20 grid of small tilted plates.
    std::vector<tactical::CollisionTriangle> field;
    for (std::int64_t row = 0; row < 20; ++row) {
        for (std::int64_t column = 0; column < 20; ++column) {
            const auto x = row * 7 - 70;
            const auto y = column * 7 - 70;
            field.push_back({at(x, y, -2), at(x + 3, y, 1), at(x, y + 3, 2)});
        }
    }
    const std::vector<tactical::CollisionMesh> tree{
        tactical::collision_mesh(field, tactical::no_hardpoint, tactical::no_hardpoint, false)};
    expect(tree[0].nodes.size() > 1 && tree[0].triangles.size() == field.size(), "DG-36: the field gets a tree");
    expect(tactical::collision_mesh(field, tactical::no_hardpoint, tactical::no_hardpoint, false) == tree[0],
        "DG-36: the tree is built the same every time");
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    const auto next = [&]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<std::int64_t>(state % 2001) - 1000;
    };
    std::size_t agreed = 0;
    std::size_t met = 0;
    for (int sample = 0; sample < 400; ++sample) {
        const math::Vec3 from{Fixed::from_raw(next() * one / 10 + 100 * one), Fixed::from_raw(next() * one / 10),
            Fixed::from_raw(next() * one / 50 + 10 * one)};
        const math::Vec3 to{Fixed::from_raw(next() * one / 10 + 100 * one), Fixed::from_raw(next() * one / 10),
            Fixed::from_raw(next() * one / 50 - 10 * one)};
        const auto fast = tactical::segment_hits_meshes(tree, all, placed, from, to);
        const auto slow = brute(tree, placed, from, to);
        agreed += fast && fast.value().has_value() == slow.has_value()
            && (!slow || fast.value()->fraction == slow->fraction);
        met += slow.has_value();
    }
    expect(agreed == 400 && met > 20, "DG-36: the tree finds what the brute force finds (" + std::to_string(met) + " met)");

    // Validation: meshes need the box and their bounds, and must stay within 4096 units.
    auto table = combat();
    table.profiles[1].meshes = {tactical::collision_mesh(plate(0, 5), tactical::no_hardpoint, tactical::no_hardpoint, false)};
    expect(!tactical::validate_combat(table), "DG-36: meshes without their bounds are refused");
    table.profiles[1].mesh_bounds = tactical::CollisionBox{at(0, -5, -5), at(0, 5, 5)};
    expect(static_cast<bool>(tactical::validate_combat(table)), "DG-36: a mesh with its box and bounds is accepted");
    table.profiles[1].meshes = {tactical::collision_mesh(plate(5000, 5), tactical::no_hardpoint, tactical::no_hardpoint, false)};
    expect(!tactical::validate_combat(table), "DG-36: a mesh beyond 4096 units is refused");
}

// #536, DG-11, DG-36, DG-38 in a session: the corvette of DG-F2 faces the shooter with a hardpoint
// plate at its hardpoint (local x 15) in front of a hull plate (local x 10). Hits destroy the
// hardpoint first; its plate then stops colliding and the hull plate takes the rest. The frigate
// gets a shield plate in front (local x 18): the shield takes every hit while it is up, and the
// hardpoint and hull behind it are untouched.
void test_mesh_hits() {
    auto table = combat();
    for (const auto index : {std::size_t{1}, std::size_t{2}}) {
        auto& profile = table.profiles[index];
        profile.meshes = {tactical::collision_mesh(plate(10, 9), tactical::no_hardpoint, tactical::no_hardpoint, false),
            tactical::collision_mesh(plate(15, 4), 0, 0, false)};
        if (index == 1) {
            profile.meshes.push_back(tactical::collision_mesh(plate(18, 9), tactical::no_hardpoint, tactical::no_hardpoint, true));
        }
        profile.mesh_bounds = tactical::CollisionBox{at(10, -9, -9), at(18, 9, 9)};
    }
    const auto make = [&](const tactical::TypeId type) {
        auto created = tactical::TacticalSession::create(
            setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, type, 2, at(300, 0), true)}), sensors(), durability(), {},
            std::nullopt, table);
        expect(static_cast<bool>(created), "mesh session is created");
        return std::move(created).value();
    };
    auto corvette = make(corvette_type);
    auto health = corvette.durability_state(2);
    for (int tick = 0; tick < 200 && health && health->hardpoints[0].raw() > 0; ++tick) {
        static_cast<void>(run(corvette, 1));
        health = corvette.durability_state(2);
    }
    expect(health && health->hardpoints[0].raw() == 0 && health->hull.raw() == 300 * one,
        "DG-11: hits on the hardpoint's mesh destroy it and leave the hull");
    auto tally = run(corvette, 200);
    health = corvette.durability_state(2);
    expect(!health || health->hull.raw() < 300 * one, "DG-38: the destroyed hardpoint's mesh lets the shots reach the hull");
    auto shielded = make(frigate_type);
    tally = run(shielded, 60);
    health = shielded.durability_state(2);
    expect(health && health->shields.raw() < 100 * one && health->hardpoints[0].raw() == 90 * one
            && health->hull.raw() == 600 * one && tally.absorbed == tally.hits && tally.hits > 0,
        "DG-38: the shield mesh takes the hits while the shield is up");
}

// #607, DG-37: the craft sphere is tested only for a step that meets the craft's world box (its
// collision box turned by the craft, then axis-aligned). A station-shaped craft (box +-10, a small
// plate for its mesh, Collision_Box_Modifier 2) sits 12 units beside the shooter's path to a
// frigate. Unturned, its box stops 2 units short of the path, so the shots pass it although the
// path runs within its sphere (radius 20), and the frigate takes them. Turned 45 degrees about Z,
// its world box reaches 14.1 units from its centre and the path crosses it: the sphere (radius
// 28.3) catches the shots. The shooter may not fire at the station, so every shot aims past it.
[[nodiscard]] std::pair<bool, bool> sphere_run(const bool turned) {
    auto table = combat();
    table.profiles[0].weapons[0].category_restrictions = 4;
    auto& station = table.profiles[3];
    station.category_bits = 4;
    station.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
    station.meshes = {tactical::collision_mesh(plate(0, 1), tactical::no_hardpoint, tactical::no_hardpoint, false)};
    station.mesh_bounds = tactical::CollisionBox{at(0, -1, -1), at(0, 1, 1)};
    station.sphere_modifier = units(2);
    auto beside = unit(3, station_type, 2, at(200, 12));
    if (turned) {
        beside.rotation = math::normalize(math::Quat{Fixed{}, Fixed{}, decimal("0.38268343"), decimal("0.92387953")}).value();
    }
    auto created = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(400, 0), true), beside}), sensors(),
        durability(), {}, std::nullopt, table);
    expect(static_cast<bool>(created), "sphere session is created");
    if (!created) return {false, false};
    auto value = std::move(created).value();
    const auto tally = run(value, 90);
    expect(tally.shots > 0 && tally.hits > 0, "the sphere run fires and hits");
    const auto frigate_health = value.durability_state(2);
    const auto station_health = value.durability_state(3);
    return {frigate_health && frigate_health->shields.raw() < 100 * one,
        station_health && station_health->shields.raw() < 100 * one};
}

void test_craft_sphere() {
    const auto [frigate_hit, station_hit] = sphere_run(false);
    expect(frigate_hit && !station_hit, "DG-37: a path within the sphere but outside the world box passes the craft");
    const auto [frigate_hit_turned, station_hit_turned] = sphere_run(true);
    expect(station_hit_turned && !frigate_hit_turned, "DG-37: the turned craft's world box lets its sphere catch the path");
    auto table = combat();
    table.profiles[3].sphere_modifier = units(1);
    expect(!tactical::validate_combat(table), "DG-37: a modifier of 1 is refused");
    table.profiles[3].sphere_modifier = units(257);
    expect(!tactical::validate_combat(table), "DG-37: a modifier above the limit is refused");
    table.profiles[3].sphere_modifier = units(250);
    expect(static_cast<bool>(tactical::validate_combat(table)), "DG-37: the Death Star's 250 is accepted");
}

// #607, DG-24: the object weapon scatters too. Its muzzle sits at the shooter's centre and the
// corvette's hardpoint (its aim point) 285 units away; with an inaccuracy of 30 (doubled: 60) and a
// range of 700, every shot lands within 285 x 60 / 700 = 24.4 units of it on each axis, height
// included.
void test_object_weapon_scatter() {
    auto table = combat();
    auto gun = laser();
    gun.hardpoint = tactical::object_weapon;
    gun.cone_width = Fixed{};
    gun.cone_height = Fixed{};
    gun.fire_a = at(0, 0);
    gun.shot->inaccuracy = {{2, units(30)}};
    table.profiles[0].weapons = {gun};
    auto created = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(300, 0), true)}), sensors(), durability(),
        {}, std::nullopt, table);
    expect(static_cast<bool>(created), "object weapon scatter session is created");
    if (!created) return;
    auto value = std::move(created).value();
    const eawr::sim::InlineExecutor executor;
    const auto radius = 285.0 * 60.0 / 700.0;
    std::size_t shots = 0;
    bool within = true;
    double widest = 0.0;
    double highest = 0.0;
    for (int tick = 0; tick < 60; ++tick) {
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "object weapon scatter step succeeds");
        if (!stepped) return;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            if (event.kind != tactical::CombatEventKind::weapon_fired) continue;
            ++shots;
            const auto offset = [&](const Fixed value, const std::int64_t centre) {
                return std::abs(static_cast<double>(value.raw() - centre * one) / static_cast<double>(one));
            };
            const auto dx = offset(event.aim.x, 285);
            const auto dy = offset(event.aim.y, 0);
            const auto dz = offset(event.aim.z, 0);
            within = within && dx <= radius + 1e-3 && dy <= radius + 1e-3 && dz <= radius + 1e-3;
            widest = std::max({widest, dx, dy, dz});
            highest = std::max(highest, dz);
        }
    }
    expect(shots >= 3 && within, "DG-24: the object weapon's shots land within its scatter radius");
    expect(widest > radius / 4 && highest > 0.5, "DG-24: the object weapon's shots scatter on every axis");
}

// W-06a (#607): the object weapon's burst clock. A three-pulse object weapon (no cone, 6 frames
// between pulses, recharge 1 s) fires at a corvette that outlasts the run.
// - With nothing to stop it, every burst fires three shots 6 frames apart, and the next burst
//   follows the last shot after 30 frames plus a synchronized draw of 0 to 10: both bounds occur.
// - With a 40-energy shot, a pool of 100 and 80 energy every recharge interval (150 frames, EN-02),
//   each later burst fires two shots and spends its third pulse unfired, since a begun burst spends
//   a pulse before the energy check; the next burst's first shot waits, unspent, for the pool
//   (EN-05). The shots come in pairs 150 frames apart. The old clock (a pulse spent only by a shot)
//   fires the held third pulse at the refill instead, and no pairs form.
void test_object_burst_clock() {
    auto table = combat();
    auto gun = laser();
    gun.hardpoint = tactical::object_weapon;
    gun.cone_width = Fixed{};
    gun.cone_height = Fixed{};
    gun.pulse_count = 3;
    gun.min_recharge_hundredths = 100;
    gun.max_recharge_hundredths = 100;
    const auto fired = [&](const tactical::CombatTable& weapons, const tactical::DurabilityTable& health, const std::uint64_t ticks) {
        std::vector<std::uint64_t> out;
        auto created = tactical::TacticalSession::create(
            setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(300, 0), true)}), sensors(), health, {},
            std::nullopt, weapons);
        expect(static_cast<bool>(created), "burst clock session is created");
        if (!created) return out;
        auto value = std::move(created).value();
        const eawr::sim::InlineExecutor executor;
        for (std::uint64_t tick = 0; tick < ticks; ++tick) {
            auto stepped = value.step(executor);
            expect(static_cast<bool>(stepped), "burst clock step succeeds");
            if (!stepped) break;
            for (const auto& event : stepped.value().snapshot->combat_events()) {
                if (event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 1) out.push_back(event.tick);
            }
        }
        return out;
    };
    auto health = durability();
    health.profiles[2].max_hull = units(1000000);

    table.profiles[0].weapons = {gun};
    const auto free = fired(table, health, 3000);
    std::vector<std::vector<std::uint64_t>> bursts;
    for (std::size_t index = 0; index < free.size(); ++index) {
        if (index == 0 || free[index] - free[index - 1] != 6) bursts.emplace_back();
        bursts.back().push_back(free[index]);
    }
    bool threes = bursts.size() > 40;
    std::uint64_t shortest = ~std::uint64_t{};
    std::uint64_t longest = 0;
    for (std::size_t index = 0; index + 1 < bursts.size(); ++index) {
        threes = threes && bursts[index].size() == 3;
        const auto gap = bursts[index + 1].front() - bursts[index].back();
        shortest = std::min(shortest, gap);
        longest = std::max(longest, gap);
    }
    expect(threes, "W-06: every burst fires its three pulses 6 frames apart (" + std::to_string(bursts.size()) + " bursts)");
    expect(shortest == 30 && longest == 40, "W-06a: the recharge is 30 frames plus a draw of 0 to 10, both bounds seen ("
            + std::to_string(shortest) + " to " + std::to_string(longest) + ")");

    auto costly = gun;
    costly.shot->energy_per_shot = units(40);
    table.profiles[0].weapons = {costly};
    health.profiles[0].powered = true;
    health.profiles[0].max_energy = units(100);
    health.profiles[0].energy_refresh = units(80);
    const auto powered = fired(table, health, 1200);
    std::vector<std::vector<std::uint64_t>> groups;
    for (std::size_t index = 0; index < powered.size(); ++index) {
        if (index == 0 || powered[index] - powered[index - 1] != 6) groups.emplace_back();
        groups.back().push_back(powered[index]);
    }
    bool pairs = groups.size() > 6;
    for (std::size_t index = 1; index < groups.size(); ++index) {
        pairs = pairs && groups[index].size() == 2;
        if (index >= 2) pairs = pairs && groups[index].front() - groups[index - 1].front() == 150;
    }
    std::string shape;
    for (const auto& group : groups) shape += ' ' + std::to_string(group.front()) + 'x' + std::to_string(group.size());
    expect(pairs, "W-06a, EN-05: a begun burst spends its third pulse unfired and the next first shot waits for energy:" + shape);
}

// #669, DG-39: the corvette's hull plate (local x 18) now stands in front of its hardpoint plate
// (local x 10). Every shot aims at the hardpoint and meets the hull plate first. With the aimed
// route the hardpoint takes every hit and the hull none until the hardpoint is destroyed; the
// shots that follow aim at the hull and wear it down. Without the route (the #536 rule alone)
// the hull plate sends every hit to the hull and the hardpoint is never touched.
void test_aimed_routes() {
    const auto make = [](const bool routed) {
        auto table = combat();
        auto& profile = table.profiles[2];
        profile.meshes = {tactical::collision_mesh(plate(18, 9), tactical::no_hardpoint, tactical::no_hardpoint, false),
            tactical::collision_mesh(plate(10, 4), 0, 0, false)};
        profile.mesh_bounds = tactical::CollisionBox{at(10, -9, -9), at(18, 9, 9)};
        if (routed) profile.aimed_routes = {0};
        auto created = tactical::TacticalSession::create(
            setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(300, 0), true)}), sensors(),
            durability(), {}, std::nullopt, table);
        expect(static_cast<bool>(created), "aimed-route session is created");
        return std::move(created).value();
    };
    auto routed = make(true);
    auto health = routed.durability_state(2);
    bool hull_untouched = true;
    for (int tick = 0; tick < 200 && health && health->hardpoints[0].raw() > 0; ++tick) {
        static_cast<void>(run(routed, 1));
        health = routed.durability_state(2);
        hull_untouched = hull_untouched && health && health->hull.raw() == 300 * one;
    }
    expect(health && health->hardpoints[0].raw() == 0 && hull_untouched,
        "DG-39: shots aimed at the hardpoint damage it through the hull mesh and leave the hull");
    static_cast<void>(run(routed, 300));
    health = routed.durability_state(2);
    expect(!health || health->hull.raw() < 300 * one, "DG-39: once it is destroyed the shots aim at the hull");

    // Without the route the first hits land on the hull; the hardpoint behind the plate is untouched.
    auto unrouted = make(false);
    health = unrouted.durability_state(2);
    for (int tick = 0; tick < 200 && health && health->hull.raw() == 300 * one; ++tick) {
        static_cast<void>(run(unrouted, 1));
        health = unrouted.durability_state(2);
    }
    expect(health && health->hardpoints[0].raw() == 90 * one && health->hull.raw() < 300 * one,
        "DG-11 without a route: the met hull mesh takes the hit (hull "
            + std::to_string(health ? health->hull.raw() / one : -1) + ", hardpoint "
            + std::to_string(health ? health->hardpoints[0].raw() / one : -1) + ")");

    // Validation: a route names a hardpoint within the type's limit.
    auto table = combat();
    table.profiles[2].aimed_routes = {static_cast<std::uint32_t>(tactical::max_hardpoints_per_type)};
    expect(!tactical::validate_combat(table), "DG-39: a route beyond the hardpoint limit is refused");
}

// #531 (space-orders OR-20, OR-25) with #669 (DG-39): a player's attack on one hardpoint takes the
// aimed route. The corvette gets a second hardpoint (1, local x 5, behind hardpoint 0 and off its
// axis), each routed to itself; the hull plate (local x 18) stands in front of both. Unordered, the
// shooter aims at the nearest hardpoint, 0. Ordered onto hardpoint 1, every shot meets the hull
// plate and still damages hardpoint 1: hardpoint 0 and the hull are untouched until it is destroyed.
void test_ordered_hardpoint_route() {
    const auto make = [] {
        auto table = combat();
        auto& profile = table.profiles[2];
        profile.hardpoints = {{0, at(15, 0), true}, {1, at(5, 6), true}};
        profile.meshes = {tactical::collision_mesh(plate(18, 9), tactical::no_hardpoint, tactical::no_hardpoint, false),
            tactical::collision_mesh(plate(15, 4), 0, 0, false)};
        profile.mesh_bounds = tactical::CollisionBox{at(5, -9, -9), at(18, 9, 9)};
        profile.aimed_routes = {0, 1};
        auto health = durability();
        health.profiles[2].hardpoints.push_back(health.profiles[2].hardpoints.front());
        auto created = tactical::TacticalSession::create(
            setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(300, 0), true)}), sensors(), health,
            {}, std::nullopt, table);
        expect(static_cast<bool>(created), "ordered-hardpoint session is created");
        return std::move(created).value();
    };
    const auto intact = [](const std::optional<tactical::DurabilityState>& health, const std::size_t index) {
        return health && health->hardpoints[index].raw() == 90 * one;
    };

    auto unordered = make();
    static_cast<void>(run(unordered, 120));
    auto health = unordered.durability_state(2);
    expect(health && health->hardpoints[0].raw() < 90 * one && intact(health, 1),
        "OR-24: unordered, the shots go to the nearest hardpoint, 0");

    auto ordered = make();
    expect(static_cast<bool>(ordered.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2, 1}})),
        "OR-20: the attack on hardpoint 1 submits");
    health = ordered.durability_state(2);
    bool others_untouched = true;
    for (int tick = 0; tick < 400 && health && health->hardpoints[1].raw() > 0; ++tick) {
        static_cast<void>(run(ordered, 1));
        health = ordered.durability_state(2);
        others_untouched = others_untouched && intact(health, 0) && health->hull.raw() == 300 * one;
    }
    expect(health && health->hardpoints[1].raw() == 0 && others_untouched,
        "OR-25, DG-39: shots on the ordered hardpoint damage it through the hull mesh, not the hull or hardpoint 0");
}

// Fixture DG-F3, out-of-range miss: the laser's projectile travels only 200 units (its range here),
// the target is 300 away: every projectile expires short of it and nothing is hit.
void test_out_of_range_miss() {
    auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(300, 0), true)}), 200);
    const auto tally = run(value, 90);
    const auto health = value.durability_state(2);
    expect(tally.shots > 0 && tally.hits == 0, "projectiles that run out of travel hit nothing");
    expect(health && health->shields.raw() == 100 * one && health->hull.raw() == 600 * one, "the target is unharmed");
    expect(value.projectiles().size() <= tally.shots, "expired projectiles leave the session");
}

// DG-33 at the range boundary, as FoC does it: each frame tests the whole step for a hit before
// the travel is counted, so the step that ends the flight can still hit up to one step past the
// range. The laser fires from the shooter's centre at a hull-less station's centre along X; its
// travel is 210, not a multiple of the speed 25, so the last step spans 200 to 225.
[[nodiscard]] Tally boundary(const std::int64_t station_x) {
    auto table = combat(210);
    table.profiles[0].weapons[0].fire_a = at(0, 0);
    table.profiles[3].collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
    auto created = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, station_type, 2, at(station_x, 0), true)}), sensors(),
        durability(), {}, std::nullopt, table);
    expect(static_cast<bool>(created), "boundary session is created");
    if (!created) return {};
    auto value = std::move(created).value();
    auto tally = run(value, 60);
    for (const auto& projectile : value.projectiles()) {
        expect(projectile.travelled < projectile.max_travel, "a projectile past its travel is gone");
    }
    return tally;
}

void test_range_boundary() {
    const auto inside = boundary(205); // face at 195
    expect(inside.shots > 0 && inside.hits > 0, "a target just inside the range is hit");
    const auto last_step = boundary(225); // face at 215: past the range, inside the last step
    expect(last_step.shots > 0 && last_step.hits > 0, "the last step hits up to one step past the range (FoC)");
    const auto beyond = boundary(245); // face at 235: past the last step
    expect(beyond.shots > 0 && beyond.hits == 0, "a target beyond the last step is never hit");
}

// A projectile flies straight: another hostile unit in its path takes the hit (DG-32), a friendly
// one does not.
void test_path() {
    auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(400, 0), true),
        unit(3, station_type, 2, at(200, 0), true), unit(4, shooter_type, 1, at(100, 30))}));
    // Only the frigate is a target: make the station unattractive by keeping it out of the scan
    // order is not needed; any hit on the station proves the path rule.
    const auto tally = run(value, 60);
    const auto frigate_health = value.durability_state(2);
    const auto station_health = value.durability_state(3);
    const auto friendly = value.durability_state(4);
    expect(tally.hits > 0 && station_health && station_health->shields.raw() < 100 * one,
        "the unit in the path takes the projectiles");
    expect(frigate_health && frigate_health->shields.raw() == 100 * one, "the unit behind it is shielded by it");
    expect(friendly && friendly->hull.raw() == 2000 * one, "a friendly unit beside the path is never hit");
}

// #636, the projectile broad phase's budget: a station with a 1200-unit box far off the lane makes
// the largest collision reach (the index query's growth) 1800 units, so every shot at the frigate
// takes the station and three corvettes beyond the weapon's range from the index as well. Their own
// reach never meets the shot's segment, so only the frigate reaches the exact tests, and only
// the frigate is hit. The third corvette sits right above the lane, 1600 units up (#718's layer
// heights): the reach test is in 3-D, so a unit planar-on-the-lane is still rejected.
void test_broad_phase_reach() {
    auto table = combat();
    table.profiles[3].collision = tactical::CollisionBox{at(-600, -600, -600), at(600, 600, 600)};
    auto created = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(300, 0), true),
            unit(3, station_type, 2, at(150, 1500), true), unit(4, corvette_type, 2, at(100, 800), true),
            unit(5, corvette_type, 2, at(250, -850), true), unit(6, corvette_type, 2, at(150, 0, 1600), true)}),
        sensors(), durability(), {}, std::nullopt, table);
    expect(static_cast<bool>(created), "broad phase session is created");
    if (!created) return;
    auto value = std::move(created).value();
    const eawr::sim::InlineExecutor executor;
    std::uint64_t steps = 0;
    std::uint64_t candidates = 0;
    std::uint64_t exact = 0;
    std::size_t hits = 0;
    for (int tick = 0; tick < 90; ++tick) {
        const auto flying = value.projectiles().size();
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "broad phase step succeeds");
        if (!stepped) return;
        expect(stepped.value().projectile_exact_tests <= flying,
            "tick " + std::to_string(tick) + ": a shot tests at most the frigate exactly");
        steps += flying;
        candidates += stepped.value().projectile_candidates;
        exact += stepped.value().projectile_exact_tests;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            hits += event.kind == tactical::CombatEventKind::projectile_hit && event.target == 2;
        }
    }
    // Each step takes the frigate, the station, the three corvettes and the shooter from the index.
    expect(steps > 0 && candidates >= 6 * steps && exact < steps, "the reach test rejects the far units");
    expect(hits > 0, "the frigate is hit");
    for (const eawr::sim::EntityId id : {3, 4, 5, 6}) {
        const auto health = value.durability_state(id);
        const auto full = id == 3 ? 100 * one : 300 * one;
        expect(health && (id == 3 ? health->shields.raw() : health->hull.raw()) == full, "a far unit is never hit");
    }
}

// #636 against DG-37's worst case: a craft's sphere hits a step that meets its world box, and a
// turned box's world box reaches sqrt(3) times its corner's length from the craft. A station-shaped
// craft with a +-100 box (corner length 173.2) and the Death Star's modifier of 250, turned 45
// degrees about X, has a world box of +-141.4 in Y and Z. It sits 140 units right and 140 up of
// the shooter's path to a frigate: the path crosses the world box 198 units from its centre, beyond
// the corner's length plus half a 25-unit step, and misses the box itself (198 units out along its
// turned diagonal). So only a reach that covers the world box keeps the craft in the exact tests;
// its sphere (radius 35,355) then takes every shot, and the frigate none.
void test_broad_phase_sphere_reach() {
    auto table = combat();
    table.profiles[0].weapons[0].category_restrictions = 4;
    auto& station = table.profiles[3];
    station.category_bits = 4;
    station.collision = tactical::CollisionBox{at(-100, -100, -100), at(100, 100, 100)};
    station.sphere_modifier = units(250);
    auto beside = unit(3, station_type, 2, at(200, 140, 140));
    beside.rotation = math::normalize(math::Quat{decimal("0.38268343"), Fixed{}, Fixed{}, decimal("0.92387953")}).value();
    auto created = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(400, 0), true), beside}), sensors(),
        durability(), {}, std::nullopt, table);
    expect(static_cast<bool>(created), "sphere reach session is created");
    if (!created) return;
    auto value = std::move(created).value();
    const auto tally = run(value, 90);
    const auto frigate_health = value.durability_state(2);
    const auto station_health = value.durability_state(3);
    expect(tally.shots > 0 && tally.hits > 0, "the sphere reach run fires and hits");
    expect(station_health && station_health->shields.raw() < 100 * one,
        "#636: the broad phase keeps a turned craft whose world box the path crosses beyond its corner");
    expect(frigate_health && frigate_health->shields.raw() == 100 * one, "the craft's sphere takes every shot");
}

// EN-05: a POWERED craft-like shooter whose object weapon (range 700, no cone, 2 shots 6 frames
// apart, recharge 1 s) costs 40 energy a shot from a pool of 100 that never refills fires twice,
// then holds fire; an unpowered one never fires it. Hardpoint shots draw nothing (EN-06).
void test_energy_weapon() {
    auto table = combat();
    auto gun = laser();
    gun.hardpoint = tactical::object_weapon;
    gun.cone_width = Fixed{};
    gun.cone_height = Fixed{};
    gun.pulse_count = 2;
    gun.min_recharge_hundredths = 100;
    gun.max_recharge_hundredths = 100;
    gun.shot->energy_per_shot = units(40);
    table.profiles[0].weapons = {gun};
    auto tables = durability();
    tables.profiles[0].powered = true;
    tables.profiles[0].max_energy = units(100);
    tables.profiles[0].energy_refresh = Fixed{};
    const auto staged = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(300, 0), true)});
    const auto started = [&](const tactical::DurabilityTable& health, const tactical::CombatTable& weapons) {
        auto created = tactical::TacticalSession::create(staged, sensors(), health, {}, std::nullopt, weapons);
        expect(static_cast<bool>(created), "energy weapon session is created");
        return std::move(created).value();
    };
    auto value = started(tables, table);
    const auto tally = run(value, 200);
    const auto pool = value.durability_state(1);
    expect(tally.shots == 2 && pool && pool->energy.raw() == 20 * one,
        "EN-05: two 40-energy shots from a pool of 100, then none (" + std::to_string(tally.shots) + " shots)");
    tables.profiles[0].powered = false;
    auto unpowered = started(tables, table);
    expect(run(unpowered, 200).shots == 0, "EN-05: without a pool the object weapon never fires");
    // EN-06: the hardpoint laser draws nothing from a pool it could empty.
    tables = durability();
    tables.profiles[0].powered = true;
    tables.profiles[0].max_energy = units(1);
    tables.profiles[0].energy_refresh = Fixed{};
    auto hardpoint = started(tables, combat());
    const auto fired = run(hardpoint, 200);
    const auto kept = hardpoint.durability_state(1);
    expect(fired.shots > 2 && kept && kept->energy.raw() == one, "EN-06: hardpoint shots cost no energy");
}

// DG-13, EN-02: each unit's shield and pool recharge on phases of their own, the first within one
// interval of its entry, then every interval. Six powered frigates lose 50 shield at tick 0.
void test_recharge_phase() {
    auto tables = durability();
    auto& ship = tables.profiles[1];
    ship.max_energy = units(1000);
    ship.energy_refresh = units(7);
    tactical::TacticalReplay replay;
    std::vector<tactical::UnitState> list;
    for (eawr::sim::EntityId id = 1; id <= 6; ++id) {
        const auto owner = static_cast<tactical::PlayerId>(1 + id % 2);
        list.push_back(unit(id, frigate_type, owner, at(static_cast<std::int64_t>(id) * 3000, 0)));
        replay.commands.push_back({{0, owner, static_cast<std::uint32_t>(id)}, {id},
            tactical::DamagePayload{units(50), tactical::hull_target}});
    }
    replay.setup = setup(list);
    std::sort(replay.commands.begin(), replay.commands.end(),
        [](const auto& left, const auto& right) { return left.key < right.key; });
    replay.final_tick_count = 500;
    auto created = tactical::TacticalSession::from_replay(replay, sensors(), tables, {}, std::nullopt, {});
    expect(static_cast<bool>(created), "phase session is created");
    if (!created) return;
    auto value = std::move(created).value();
    std::vector<std::vector<std::uint64_t>> shield_ticks(7);
    std::vector<std::vector<std::uint64_t>> energy_ticks(7);
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) {
        std::vector<tactical::DurabilityState> before(7);
        for (eawr::sim::EntityId id = 1; id <= 6; ++id) before[id] = *value.durability_state(id);
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "phase step succeeds");
        if (!stepped) break;
        for (eawr::sim::EntityId id = 1; id <= 6; ++id) {
            const auto after = *value.durability_state(id);
            if (after.shields > before[id].shields) shield_ticks[id].push_back(stepped.value().completed_tick);
            if (after.energy > before[id].energy) energy_ticks[id].push_back(stepped.value().completed_tick);
        }
    }
    std::vector<std::uint64_t> phases;
    for (eawr::sim::EntityId id = 1; id <= 6; ++id) {
        const auto& shields = shield_ticks[id];
        bool periodic = shields.size() == 5;
        for (std::size_t k = 1; k < shields.size(); ++k) periodic = periodic && shields[k] - shields[k - 1] == 90;
        expect(periodic && !shields.empty() && shields.front() <= 90,
            "DG-13: unit " + std::to_string(id) + " regains its 50 shield in 5 recharges 90 frames apart");
        const auto& energy = energy_ticks[id];
        bool every = !energy.empty();
        for (std::size_t k = 1; k < energy.size(); ++k) every = every && (energy[k] - energy[k - 1]) % 150 == 0;
        expect(every, "EN-02: unit " + std::to_string(id) + " refills its pool on a 150-frame phase");
        phases.push_back(shields.empty() ? 0 : shields.front() % 90);
    }
    std::sort(phases.begin(), phases.end());
    expect(std::unique(phases.begin(), phases.end()) - phases.begin() > 1, "DG-13: units recharge on different phases");
}

// --- Missiles (MS-01 to MS-07, #361) ------------------------------------------------------------

// A corvette that crawls at 3 units per frame, like FoC's corvettes.
[[nodiscard]] tactical::MotionTable crawling_motion() {
    tactical::MotionTable motion;
    motion.rules = {units(15), units(300)};
    tactical::MotionProfile profile;
    profile.type_id = corvette_type;
    profile.max_speed = units(3);
    profile.acceleration = units(3);
    profile.deceleration = units(3);
    profile.rate_of_turn = units(90);
    profile.turn_in_place_slowdown = units(1);
    motion.profiles.push_back(profile);
    return motion;
}

// A concussion-missile-like hardpoint: one shot a second at 7 units per frame, travel 1400, turning
// 3 degrees a frame when it homes.
[[nodiscard]] tactical::CombatTable missile_combat(const bool homing) {
    auto table = combat(1400);
    auto& weapon = table.profiles[0].weapons.front();
    weapon.pulse_count = 1;
    weapon.min_recharge_hundredths = 100;
    weapon.max_recharge_hundredths = 100;
    weapon.shot->speed = units(7);
    weapon.shot->homing = homing;
    weapon.shot->turn_rate = homing ? units(3) : Fixed{};
    return table;
}

struct MissileRun {
    std::size_t shots{};
    std::size_t hits{};
    bool turn_bounded{true};
    bool homing_seen{};
};

// With `weave`, the corvette (unit 2) turns back every 60 ticks between north and south.
[[nodiscard]] MissileRun missile_run(tactical::TacticalSession& value, const std::uint64_t ticks, const bool weave = false) {
    MissileRun result;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        if (weave && tick > 0 && tick % 60 == 0) {
            const auto north = (tick / 60) % 2 == 0;
            expect(static_cast<bool>(value.submit({{tick, 2, tick}, {2}, tactical::MovePayload{at(600, north ? 1500 : -1500)}})),
                "the corvette's turn is queued");
        }
        std::vector<tactical::Projectile> before(value.projectiles().begin(), value.projectiles().end());
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "missile step succeeds");
        if (!stepped) break;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            result.shots += event.kind == tactical::CombatEventKind::weapon_fired;
            result.hits += event.kind == tactical::CombatEventKind::projectile_hit;
        }
        for (const auto& after : value.projectiles()) {
            const auto earlier = std::find_if(before.begin(), before.end(),
                [&](const tactical::Projectile& entry) { return entry.id == after.id; });
            if (earlier == before.end()) continue;
            result.homing_seen = result.homing_seen || after.homing;
            const auto turn = [](const Fixed a, const Fixed b) {
                auto d = std::fmod(static_cast<double>(a.raw() - b.raw()) / static_cast<double>(one), 360.0);
                if (d > 180.0) d -= 360.0;
                if (d <= -180.0) d += 360.0;
                return std::abs(d);
            };
            result.turn_bounded = result.turn_bounded && turn(after.yaw, earlier->yaw) <= 3.0 + 1e-6
                && turn(after.pitch, earlier->pitch) <= 3.0 + 1e-6;
        }
    }
    return result;
}

[[nodiscard]] tactical::TacticalSession crawling_session(const bool homing) {
    auto staged = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(600, 0), true)});
    auto created = tactical::TacticalSession::create(
        staged, sensors(), durability(), crawling_motion(), std::nullopt, missile_combat(homing));
    expect(static_cast<bool>(created), "missile session is created");
    auto value = std::move(created).value();
    expect(static_cast<bool>(value.submit({{0, 2, 0}, {2}, tactical::MovePayload{at(600, 1500)}})),
        "the corvette's move is queued");
    return value;
}

// MS-01 to MS-04: a missile fired at a corvette weaving across its path turns after it, at most 3
// degrees a frame, and hits; the same shots flown straight (MS-06) lead only the corvette's last
// move (W-10), so they miss when it turns back.
void test_missile_homing() {
    auto homing = crawling_session(true);
    const auto steered = missile_run(homing, 300, true);
    auto straight = crawling_session(false);
    const auto flown = missile_run(straight, 300, true);
    std::cout << "MS: homing missiles hit " << steered.hits << " of " << steered.shots << ", straight ones "
              << flown.hits << " of " << flown.shots << '\n';
    expect(steered.homing_seen && steered.turn_bounded, "MS-04: a missile turns at most its Max_Rate_Of_Turn a frame");
    expect(steered.shots > 3 && steered.hits * 2 >= steered.shots,
        "MS-03: homing missiles hit a weaving corvette (" + std::to_string(steered.hits) + " of "
            + std::to_string(steered.shots) + ")");
    expect(flown.hits * 2 < steered.hits, "MS-06: projectiles that do not home miss a turning corvette ("
            + std::to_string(flown.hits) + " hits)");
}

// MS-05: when the target leaves, a missile keeps its facing and never takes another target, even
// with a hostile unit off its bow; it flies out its travel.
void test_missile_lock_loss() {
    auto staged = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(600, 0), true),
        unit(3, frigate_type, 2, at(300, 250), true)});
    // The corvette is the intended first lock; the frigate is only the decoy for the lock-loss
    // check, so a priority set picks it over the frigate regardless of the collection order
    // (#469, space-targeting CO-11: an equal-priority tie is order-dependent). The frigate's
    // priority stays off 1.0 (R-09's "no more searching needed" sentinel), or the search could
    // stop on it before ever weighing the corvette.
    auto table = missile_combat(true);
    tactical::PrioritySet priorities;
    priorities.rows = {{frigate_type, decimal("0.5")}, {corvette_type, units(0)}};
    priorities.unlisted = decimal("0.5");
    table.priority_sets.push_back(priorities);
    table.profiles[0].priority_set = static_cast<std::uint32_t>(table.priority_sets.size() - 1);
    auto created = tactical::TacticalSession::create(staged, sensors(), durability(), {}, std::nullopt, table);
    expect(static_cast<bool>(created), "lock-loss session is created");
    if (!created) return;
    auto value = std::move(created).value();
    const eawr::sim::InlineExecutor executor;
    std::optional<tactical::Projectile> first;
    for (int tick = 0; tick < 200 && !first; ++tick) {
        static_cast<void>(value.step(executor));
        for (const auto& projectile : value.projectiles()) {
            if (projectile.target == 2) {
                first = projectile;
                break;
            }
        }
    }
    expect(first.has_value() && first->locked, "MS-02: the missile flies locked on the corvette");
    if (!first) return;
    expect(static_cast<bool>(value.stage_remove(2)), "the corvette is removed");
    std::optional<tactical::Projectile> last;
    bool straight = true;
    for (int tick = 0; tick < 60; ++tick) {
        static_cast<void>(value.step(executor));
        const auto flying = value.projectiles();
        const auto found = std::find_if(flying.begin(), flying.end(),
            [&](const tactical::Projectile& entry) { return entry.id == first->id; });
        if (found == flying.end()) break;
        if (last) straight = straight && found->yaw == last->yaw && found->pitch == last->pitch && found->step == last->step;
        last = *found;
    }
    expect(last.has_value() && !last->locked && straight, "MS-05: without its target the missile flies straight on");
}

// --- Determinism and the golden pin -----------------------------------------------------------

[[nodiscard]] tactical::TacticalReplay battle() {
    tactical::TacticalReplay replay;
    std::vector<tactical::UnitState> list;
    eawr::sim::EntityId id = 1;
    for (std::int64_t row = 0; row < 5; ++row) {
        for (std::int64_t column = 0; column < 3; ++column) {
            list.push_back(unit(id++, shooter_type, 1, at(-250 - column * 60, row * 70 - 140)));
            const auto type = static_cast<tactical::TypeId>(2 + (row + column) % 3);
            list.push_back(unit(id++, (row + column) % 4 == 0 ? shooter_type : type, 2, at(250 + column * 60, row * 70 - 140), true));
        }
    }
    replay.setup = setup(list);
    replay.commands.push_back({{30, 1, 0}, {1, 3}, tactical::AttackPayload{4}});
    replay.commands.push_back({{60, 2, 0}, {6}, tactical::DamagePayload{units(150), tactical::hull_target}});
    replay.commands.push_back({{90, 1, 1}, {1}, tactical::StopPayload{}});
    replay.final_tick_count = 240;
    return replay;
}

// The battle's shooters also carry a missile battery (MS-01 to MS-05) on a second hardpoint, so
// homing flight runs under every worker count.
[[nodiscard]] tactical::CombatTable battle_combat() {
    auto table = combat();
    auto launcher = missile_combat(true).profiles[0].weapons.front();
    launcher.hardpoint = 1;
    launcher.fire_a = at(-10, 5, 3);
    table.profiles[0].weapons.push_back(launcher);
    return table;
}

struct Trace {
    std::vector<std::string> rows; // tick,state,snapshot
    std::size_t hits{};
    std::size_t deaths{};
    std::uint64_t candidates{}; // #636: the projectile broad phase's work
    std::uint64_t exact_tests{};
    bool projectile_phase{};
    bool projectiles_published{true};
};

class PhaseProbe final : public eawr::sim::PartitionExecutor {
public:
    explicit PhaseProbe(const eawr::sim::PartitionExecutor& inner) : inner_(inner) {}
    [[nodiscard]] std::size_t worker_count() const noexcept override { return inner_.worker_count(); }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t partitions, const std::function<void(std::size_t)>& work) const override {
        return inner_.execute(partitions, work);
    }
    [[nodiscard]] eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t partitions,
        const std::function<void(std::size_t)>& work) const override {
        names.emplace_back(phase);
        return inner_.execute_phase(phase, partitions, work);
    }
    mutable std::vector<std::string> names;

private:
    const eawr::sim::PartitionExecutor& inner_;
};

[[nodiscard]] Trace trace(tactical::TacticalSession value, const eawr::sim::PartitionExecutor& executor,
    const std::uint64_t ticks, const bool scramble) {
    Trace result;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        if (scramble) value.scramble_storage_for_testing();
        const PhaseProbe probe(executor);
        const bool flying = !value.projectiles().empty();
        auto stepped = value.step(probe);
        expect(static_cast<bool>(stepped), "battle step succeeds");
        if (!stepped) break;
        // The projectile phase runs partitioned, after targeting and before the unit systems.
        if (flying) {
            const auto find = [&](const std::string_view name) {
                return static_cast<std::size_t>(std::find(probe.names.begin(), probe.names.end(), name) - probe.names.begin());
            };
            result.projectile_phase = true;
            expect(find("targeting") < find("projectiles") && find("projectiles") < find("unit-systems")
                    && find("unit-systems") < probe.names.size(),
                "tick " + std::to_string(tick) + ": the projectiles phase runs between targeting and unit-systems");
        }
        result.rows.push_back(std::to_string(stepped.value().completed_tick) + ',' + stepped.value().state_sha256 + ','
            + stepped.value().snapshot->sha256());
        result.candidates += stepped.value().projectile_candidates;
        result.exact_tests += stepped.value().projectile_exact_tests;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            result.hits += event.kind == tactical::CombatEventKind::projectile_hit;
        }
        // #80: the snapshot publishes the projectiles in flight after the tick.
        const auto published = stepped.value().snapshot->projectiles();
        const auto flying_now = value.projectiles();
        result.projectiles_published = result.projectiles_published
            && std::equal(published.begin(), published.end(), flying_now.begin(), flying_now.end());
        for (const auto& event : stepped.value().snapshot->events()) {
            result.deaths += event.kind == tactical::EventKind::unit_destroyed;
        }
    }
    return result;
}

void test_battle(const std::filesystem::path& fixtures, const bool update) {
    const auto replay = battle();
    const auto golden = fixtures / "tactical-damage.hashes.csv";
    const auto created = [&] {
        auto value = tactical::TacticalSession::from_replay(replay, sensors(), durability(), {}, std::nullopt, battle_combat());
        expect(static_cast<bool>(value), "battle session is created");
        return std::move(value).value();
    };
    const eawr::sim::InlineExecutor inline_executor;
    const auto reference = trace(created(), inline_executor, replay.final_tick_count, false);
    expect(reference.hits > 50 && reference.deaths > 0 && reference.projectile_phase, "the battle hits and kills");
    expect(reference.projectiles_published, "#80: every snapshot publishes the projectiles in flight");
    if (update) {
        std::ofstream output(golden, std::ios::binary);
        output << "tick,state_sha256,snapshot_sha256\n";
        for (const auto& row : reference.rows) output << row << '\n';
        std::cout << "wrote " << golden.string() << '\n';
    } else {
        std::ifstream input(golden, std::ios::binary);
        std::string line;
        std::getline(input, line);
        std::vector<std::string> pinned;
        while (std::getline(input, line)) pinned.push_back(line);
        expect(pinned == reference.rows, "the battle matches tactical-damage.hashes.csv");
    }
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto traced = trace(created(), executor, replay.final_tick_count, false);
        expect(traced.rows == reference.rows, "battle with " + std::to_string(workers) + " workers matches");
        expect(traced.candidates == reference.candidates && traced.exact_tests == reference.exact_tests
                && reference.exact_tests > 0 && reference.exact_tests <= reference.candidates,
            "#636: the broad phase's work counts match with " + std::to_string(workers) + " workers");
    }
    const eawr::platform::ThreadWorkerAdapter four(4);
    expect(trace(created(), four, replay.final_tick_count, true).rows == reference.rows,
        "battle with scrambled storage matches");
    auto written = tactical::write_replay(replay);
    expect(static_cast<bool>(written), "battle replay is written");
    if (written) {
        auto parsed = tactical::parse_replay(written.value());
        expect(static_cast<bool>(parsed), "battle replay parses");
        if (parsed) {
            auto again = tactical::TacticalSession::from_replay(parsed.value(), sensors(), durability(), {}, std::nullopt, battle_combat());
            expect(static_cast<bool>(again)
                    && trace(std::move(again).value(), inline_executor, replay.final_tick_count, false).rows == reference.rows,
                "a written-and-parsed battle replay matches");
        }
    }
}

void test_validation() {
    auto table = durability();
    expect(static_cast<bool>(tactical::validate_durability(table)), "the test durability table is valid");
    table.damage->shield_recharge_frames = 0;
    expect(!tactical::validate_durability(table), "a zero recharge interval is rejected");
    table = durability();
    table.damage->armor_mods.pop_back();
    expect(!tactical::validate_durability(table), "a short armor table is rejected");
    table = durability();
    table.profiles[1].shield_armor_type = 9;
    expect(!tactical::validate_durability(table), "an unknown armor index is rejected");
    table = durability();
    std::swap(table.damage->diminishing[0], table.damage->diminishing[1]);
    expect(!tactical::validate_durability(table), "an unordered curve is rejected");
    // The bounds keep the pipeline in Q24: the largest projectile damage through the largest
    // curve value and armor multipliers destroys a unit instead of failing the tick.
    table = durability();
    const auto most = units(tactical::max_damage_multiplier);
    for (auto& point : table.damage->diminishing) point.y = most;
    for (auto& value : table.damage->armor_mods) value = most;
    expect(static_cast<bool>(tactical::validate_durability(table)), "the largest multipliers are valid");
    const tactical::Hit largest{units(tactical::max_durability_health), 0, true, true, true, tactical::hull_target};
    for (const auto& profile : {table.profiles[1], table.profiles[2]}) {
        auto state = tactical::full_durability(profile);
        const auto outcome = tactical::apply_hit(profile, *table.damage, state, largest, 10);
        expect(outcome && state.hull.raw() == 0 && state.shields.raw() == 0,
            "the largest validated hit on type " + std::to_string(profile.type_id) + " stays in range");
    }
    table.damage->armor_mods[0] = Fixed::from_raw(most.raw() + 1);
    expect(!tactical::validate_durability(table), "an armor multiplier above the bound is rejected");
    table = durability();
    table.damage->diminishing[3].y = Fixed::from_raw(most.raw() + 1);
    expect(!tactical::validate_durability(table), "a curve value above the bound is rejected");
    // A spline that overshoots its points is capped at the bound: 0, 256, 256, 0 at whole
    // seconds peaks near 294 at 1.5 s (45 frames).
    table = durability();
    table.damage->diminishing = {{units(0), units(0)}, {units(1), most}, {units(2), most}, {units(3), units(0)}};
    expect(static_cast<bool>(tactical::validate_durability(table)), "an overshooting curve is valid");
    const auto capped = tactical::diminishing_factor(*table.damage, 45);
    expect(capped && capped.value() == most, "an overshooting curve is capped at the bound");
    // Points a raw step apart would overflow the spline in a hit: rejected up front.
    table.damage->diminishing = {{Fixed::from_raw(0), units(0)}, {Fixed::from_raw(1), most}, {Fixed::from_raw(2), units(0)}};
    expect(!tactical::validate_durability(table), "a curve that overflows between its points is rejected");
    auto weapons = combat();
    weapons.profiles[0].weapons[0].shot->speed = Fixed{};
    expect(!tactical::validate_combat(weapons), "a projectile without speed is rejected");
    weapons = combat();
    weapons.profiles[1].collision = tactical::CollisionBox{at(5, 0), at(-5, 0)};
    expect(!tactical::validate_combat(weapons), "an inverted collision box is rejected");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: damage_tests <fixtures directory> [--update]\n";
        return 2;
    }
    const bool update = argc > 2 && std::string_view(argv[2]) == "--update";
    test_validation();
    test_curve();
    test_hit_pipeline();
    test_diminishing_gates();
    test_out_of_combat();
    test_arrival_vulnerability();
    test_recharge();
    test_energy();
    test_segment();
    test_shield_loss();
    test_arrival_in_battle();
    test_hardpoint_destroyed();
    test_meshes();
    test_mesh_hits();
    test_craft_sphere();
    test_object_weapon_scatter();
    test_object_burst_clock();
    test_aimed_routes();
    test_ordered_hardpoint_route();
    test_out_of_range_miss();
    test_range_boundary();
    test_path();
    test_broad_phase_reach();
    test_broad_phase_sphere_reach();
    test_energy_weapon();
    test_recharge_phase();
    test_missile_homing();
    test_missile_lock_loss();
    test_battle(argv[1], update);
    if (failures != 0) {
        std::cerr << failures << " damage contract test(s) failed\n";
        return 1;
    }
    std::cout << "damage contracts passed\n";
    return 0;
}
