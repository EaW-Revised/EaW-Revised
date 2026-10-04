#include "damage_support.hpp"

namespace damage_test_support {

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
    auto storm_rules = value;
    storm_rules.ion_storm_disable_seconds = units(5);
    state = tactical::full_durability(profile);
    state.ion_storm_contact = 100;
    expect(tactical::in_ion_storm(storm_rules, state, 249) && !tactical::in_ion_storm(storm_rules, state, 250),
        "WHZ-31: storm predicate uses a strict five-second contact-age window");
    hit.amount = units(10);
    outcome = tactical::apply_hit(profile, storm_rules, state, hit, 100);
    expect(outcome && outcome.value().absorbed == Fixed{} && outcome.value().storm_shield_branch
        && state.shields == units(100) && state.hull == units(570) && !state.depleted_frame,
        "WHZ-32: storm damage bypasses absorption and preserves the shield pool/depletion state");
    hit.shield_damage = false;
    state.last_hit_frame.reset();
    outcome = tactical::apply_hit(profile, storm_rules, state, hit, 101);
    expect(outcome && !outcome.value().storm_shield_branch && state.shields == units(100),
        "WHZ-32: a projectile without shield damage never enters the storm shield branch");
    state.shields = units(40);
    expect(tactical::recharge_shields(profile, storm_rules, state, 102).has_value() && state.shields == units(50),
        "WHZ-30: storm membership does not block normal shield recharge");
    state.ion_storm_contact.reset();
    expect(!tactical::in_ion_storm(storm_rules, state, 103), "WHZ-31: clearing contact ends the storm immediately");
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

// #530 PU-38, PU-39 in a battle: player 1 buys an armed unit and brings it in at (0, 0), 300 units
// from player 2's shooter. It holds its fire until its arrival ends (frame 150); the first hit it
// takes before then does what apply_hit gives for a defense modifier of -3.
void test_arrival_in_battle() {
    const auto staged = setup({unit(1, station_type, 1, at(-2000, 0)), unit(2, shooter_type, 2, at(300, 0), true)});
    tactical::EconomyRules economy;
    economy.players = {{1, units(1000), 25, false, Fixed{}}, {2, units(1000), 25, true, units(180)}};
    tactical::StationMenu menu;
    menu.station = station_type;
    menu.faction = 1;
    menu.options = {{shooter_type, tactical::BuildKind::unit, tactical::BuildQueue::units, units(100), 1, 1, 1, true}};
    economy.menus = {std::move(menu)};
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


} // namespace damage_test_support
