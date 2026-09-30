#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// P2-13 (#76): unit abilities (docs/behaviour/space-abilities.md). The pure rules are checked with
// the M2 fleet's FoC values for each modelled ability (activation, duration, recharge and the
// multipliers); the session cases cover TURBO speeding up a move under way (AB-24), the DEFEND
// stand-in for a non-human and a human owner (AB-41 to AB-43), command rejections, and the
// same hashes for 1, 2, 4 and 8 workers and for the recorded replay.
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

// The M2 fleet's Unit_Ability data (spaceunitsfrigates.xml, spaceunitscorvettes.xml,
// spaceunitsfighters.xml), seconds truncated to frames (AB-04).
[[nodiscard]] tactical::AbilityProfile defend() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::defend;
    profile.expiration_frames = 450;
    profile.recharge_frames = 1800;
    profile.modifiers.weapon_delay = units(1);
    profile.modifiers.shield_regen = units(1);
    profile.modifiers.shield_regen_interval = decimal("0.1");
    profile.modifiers.energy_regen_interval = decimal("0.1");
    profile.modifiers.energy_regen = units(3);
    profile.modifiers.speed = decimal("0.8");
    profile.supports_autofire = true;
    return profile;
}
[[nodiscard]] tactical::AbilityProfile turbo() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::turbo;
    profile.expiration_frames = 600;
    profile.recharge_frames = 1500;
    profile.modifiers.weapon_delay = units(3);
    profile.modifiers.shield_regen = Fixed{};
    profile.modifiers.energy_regen = units(1);
    profile.modifiers.speed = units(2);
    return profile;
}
[[nodiscard]] tactical::AbilityProfile tartan_power() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::power_to_weapons;
    profile.expiration_frames = 210;
    profile.recharge_frames = 1800;
    profile.modifiers.speed = decimal("0.5");
    profile.modifiers.shield_regen = units(-25);
    profile.modifiers.energy_regen = units(1);
    profile.modifiers.weapon_delay = decimal("0.2");
    return profile;
}
[[nodiscard]] tactical::AbilityProfile acclamator_power() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::power_to_weapons;
    profile.expiration_frames = 600;
    profile.recharge_frames = 1800;
    profile.modifiers.weapon_delay = decimal("0.5");
    profile.modifiers.shield_regen = units(-3);
    profile.modifiers.energy_regen = units(1);
    profile.modifiers.speed = decimal("0.5");
    return profile;
}
[[nodiscard]] tactical::AbilityProfile spoiler_lock() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::spoiler_lock;
    profile.modifiers.weapon_delay = units(3);
    profile.modifiers.shield_regen = units(3);
    profile.modifiers.energy_regen = units(3);
    profile.modifiers.speed = decimal("1.3");
    return profile;
}

[[nodiscard]] tactical::UnitAbilityProfile unit_of(const tactical::TypeId type, const tactical::AbilityProfile& ability,
    const bool script = false) {
    return {type, {ability}, script};
}

const tactical::AbilityGate open_gate{true, true, false, true};

void test_names_and_validation() {
    expect(tactical::ability_kind("DEFEND") == tactical::AbilityKind::defend, "DEFEND");
    expect(tactical::ability_kind("power_to_weapons") == tactical::AbilityKind::power_to_weapons,
        "the Acclamator's lower-case spelling");
    expect(tactical::ability_kind("Spoiler_Lock") == tactical::AbilityKind::spoiler_lock, "any case");
    expect(tactical::ability_kind("HUNT") == tactical::AbilityKind::none, "HUNT is cut (AB-03)");
    expect(tactical::ability_kind("ION_CANNON_SHOT") == tactical::AbilityKind::ion_cannon_shot,
        "ION_CANNON_SHOT is modelled since #561 (AB-60)");
    expect(tactical::to_string(tactical::AbilityKind::turbo) == "TURBO", "TURBO's name");

    tactical::AbilityTable table;
    table.profiles = {unit_of(7, turbo()), unit_of(9, defend(), true)};
    table.humans = {1};
    expect(static_cast<bool>(tactical::validate_abilities(table)), "an M2-shaped table is valid");
    auto unsorted = table;
    std::swap(unsorted.profiles[0], unsorted.profiles[1]);
    expect(!tactical::validate_abilities(unsorted), "type IDs must increase");
    auto doubled = table;
    doubled.profiles[0].abilities.push_back(turbo());
    expect(!tactical::validate_abilities(doubled), "two abilities of one kind are rejected");
    auto stalled = table;
    stalled.profiles[0].abilities[0].modifiers.speed = Fixed{};
    expect(!tactical::validate_abilities(stalled), "a zero speed multiplier is rejected");
    auto huge = table;
    huge.profiles[0].abilities[0].modifiers.shield_regen = units(65);
    expect(!tactical::validate_abilities(huge), "a multiplier beyond 64 is rejected");
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}};
    expect(!tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, unsorted),
        "a session rejects an invalid ability table");
}

void test_timers() {
    // AB-11, AB-12: TURBO lasts 600 frames, then recharges for 1500.
    const auto profile = turbo();
    tactical::AbilitySlot slot;
    expect(tactical::ability_ready(profile, slot, open_gate, 0), "a fresh ability is ready");
    const auto on = tactical::activate_ability(profile, slot, open_gate, 100);
    expect(on.changed && on.speed && slot.active && slot.expires_tick == 700, "TURBO switches on until tick 700");
    expect(!tactical::activate_ability(profile, slot, open_gate, 101).changed, "an active ability does not switch on again");
    expect(tactical::ability_ready(profile, slot, open_gate, 101), "an active ability reads as ready (AB-13)");
    tactical::UnitAbilityProfile unit = unit_of(7, profile);
    tactical::AbilityState state;
    state.slots = {slot};
    expect(!tactical::expire_abilities(unit, state, 699).changed && state.slots[0].active, "still on at tick 699");
    const auto expired = tactical::expire_abilities(unit, state, 700);
    expect(expired.changed && expired.speed && !state.slots[0].active && state.slots[0].ready_tick == 2200,
        "it runs out at tick 700 and recharges until tick 2200");
    expect(!tactical::ability_ready(profile, state.slots[0], open_gate, 2199), "recharging at tick 2199");
    expect(tactical::ability_ready(profile, state.slots[0], open_gate, 2200), "ready at tick 2200");

    // AB-12: DEFEND ended early recharges for the share of its 450 frames that ran.
    const auto shield = defend();
    tactical::AbilitySlot early;
    static_cast<void>(tactical::activate_ability(shield, early, open_gate, 1000));
    static_cast<void>(tactical::deactivate_ability(shield, early, 1150));
    expect(early.ready_tick == 1150 + 600, "150 of 450 frames: a third of 1800 (600 frames)");
    tactical::AbilitySlot odd;
    static_cast<void>(tactical::activate_ability(shield, odd, open_gate, 0));
    static_cast<void>(tactical::deactivate_ability(shield, odd, 1));
    expect(odd.ready_tick == 1 + 4, "1 of 450 frames: 1800 / 450 = 4 frames");
    tactical::AbilitySlot instant;
    static_cast<void>(tactical::activate_ability(shield, instant, open_gate, 50));
    static_cast<void>(tactical::deactivate_ability(shield, instant, 50));
    expect(instant.ready_tick == 50, "ended in its first frame: no recharge");

    // SPOILER_LOCK has no time limit and no recharge: a free toggle (the owner's S-foil note).
    const auto foils = spoiler_lock();
    tactical::AbilitySlot lock;
    for (std::uint64_t tick = 10; tick < 20; tick += 2) {
        expect(tactical::activate_ability(foils, lock, open_gate, tick).changed && lock.expires_tick == 0,
            "the S-foils lock at once");
        expect(tactical::deactivate_ability(foils, lock, tick + 1).changed && lock.ready_tick <= tick + 1,
            "and open again at once");
    }

    // AB-14, AB-16: DEFEND needs an online shield outside its depletion effect; TURBO and
    // SPOILER_LOCK need engines.
    tactical::AbilitySlot gated;
    expect(!tactical::ability_ready(shield, gated, {false, true, false, true}, 0), "no shield: no DEFEND");
    expect(!tactical::ability_ready(shield, gated, {true, false, false, true}, 0), "generators lost: no DEFEND");
    expect(!tactical::ability_ready(shield, gated, {true, true, true, true}, 0), "depleted shield: no DEFEND");
    expect(!tactical::ability_ready(profile, gated, {true, true, false, false}, 0), "engines lost: no TURBO");
    expect(!tactical::ability_ready(foils, gated, {true, true, false, false}, 0), "engines lost: no S-foil lock");
    expect(tactical::ability_ready(tartan_power(), gated, {false, false, true, false}, 0), "POWER_TO_WEAPONS has no gate");
}

void test_multipliers() {
    // AB-20: one active ability's multipliers; none active: 1.
    tactical::UnitAbilityProfile corvette = unit_of(7, turbo());
    tactical::AbilityState state = tactical::initial_abilities(corvette);
    using M = tactical::AbilityModifier;
    expect(tactical::ability_multiplier(corvette, state, M::speed).raw() == one, "inactive: speed 1");
    static_cast<void>(tactical::activate_ability(corvette.abilities[0], state.slots[0], open_gate, 0));
    expect(tactical::ability_multiplier(corvette, state, M::speed).raw() == 2 * one, "TURBO: speed 2");
    expect(tactical::ability_multiplier(corvette, state, M::weapon_delay).raw() == 3 * one, "TURBO: weapon delay 3");
    expect(tactical::ability_multiplier(corvette, state, M::shield_regen).raw() == 0, "TURBO: no shield regen");
    expect(tactical::ability_multiplier(corvette, state, M::shield_regen_interval).raw() == one, "unauthored: 1");
    // Two abilities multiply (a primary and a secondary; no M2 type has two).
    tactical::UnitAbilityProfile both{8, {turbo(), spoiler_lock()}, false};
    tactical::AbilityState two = tactical::initial_abilities(both);
    static_cast<void>(tactical::activate_ability(both.abilities[0], two.slots[0], open_gate, 0));
    static_cast<void>(tactical::activate_ability(both.abilities[1], two.slots[1], open_gate, 0));
    expect(tactical::ability_multiplier(both, two, M::speed) == decimal("2.6"), "2 x 1.3 = 2.6");

    // AB-21: the weapon delay lengthens a burst's gap; the recharge after it only shortens.
    expect(tactical::scaled_weapon_delay(90, units(3), false) == 270, "S-foils locked: a 90-frame gap takes 270");
    expect(tactical::scaled_weapon_delay(90, units(3), true) == 90, "a full recharge is never lengthened");
    expect(tactical::scaled_weapon_delay(90, decimal("0.2"), true) == 18, "the Tartan's POWER_TO_WEAPONS: 90 -> 18");
    expect(tactical::scaled_weapon_delay(45, acclamator_power().modifiers.weapon_delay, true) == 23, "the Acclamator's: 45 -> 22.5, rounded up");
    expect(tactical::scaled_weapon_delay(7, decimal("0.5"), false) == 4, "half of 7 rounds half up");
    // AB-22, AB-23: DEFEND's 0.1 intervals: shields every 9 frames, energy every 15.
    expect(tactical::scaled_interval(90, decimal("0.1")) == 9, "90 x 0.1 = 9 frames");
    expect(tactical::scaled_interval(150, decimal("0.1")) == 15, "150 x 0.1 = 15 frames");
    expect(tactical::scaled_interval(3, decimal("0.1")) == 1, "at least one frame");
}

// --- Sessions ----------------------------------------------------------------------------------

constexpr tactical::TypeId corvette_type = 11;
constexpr tactical::TypeId frigate_type = 12;
constexpr tactical::TypeId plain_type = 13;

[[nodiscard]] tactical::MotionTable motion() {
    const tactical::MotionProfile corvette{corvette_type, decimal("3.72"), decimal("0.06"), decimal("0.06"),
        decimal("1.5"), units(2), decimal("0.24"), units(15)};
    const tactical::MotionProfile frigate{frigate_type, decimal("2.64"), decimal("0.048"), decimal("0.048"),
        decimal("0.84"), units(3), decimal("0.24"), units(5)};
    return {{units(15), units(300)}, {corvette, frigate}, std::nullopt, {}, {}};
}

[[nodiscard]] tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
    tactical::DamageRules damage;
    damage.shield_recharge_frames = 90;
    damage.depleted_disable_seconds = units(5);
    damage.depleted_regen_cap = decimal("0.25");
    damage.damage_types = 1;
    damage.armor_types = 1;
    damage.armor_mods = {units(1)};
    damage.energy_recharge_frames = 150;
    damage.energy_to_shield = units(5);
    table.damage = damage;
    for (const auto type : {corvette_type, frigate_type, plain_type}) {
        tactical::DurabilityProfile profile;
        profile.type_id = type;
        profile.max_hull = units(3600);
        profile.max_shields = units(700);
        profile.shield_refresh = units(50);
        profile.armor_type = 0;
        profile.shield_armor_type = 0;
        table.profiles.push_back(profile);
    }
    return table;
}

[[nodiscard]] tactical::AbilityTable abilities(const std::vector<tactical::PlayerId>& humans) {
    tactical::AbilityTable table;
    table.profiles = {unit_of(corvette_type, turbo()), unit_of(frigate_type, defend(), true)};
    table.humans = humans;
    return table;
}

[[nodiscard]] tactical::TacticalSetup setup() {
    tactical::TacticalSetup result;
    result.seed = 76;
    result.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    result.units = {
        {1, corvette_type, 1, at(-1800, -1500), math::identity_quat(), {}},
        {2, frigate_type, 1, at(0, 2000), math::identity_quat(), {}},
        {3, plain_type, 2, at(4000, 4000), math::identity_quat(), {}},
    };
    return result;
}

[[nodiscard]] tactical::PlayerCommand command(const std::uint64_t tick, const tactical::PlayerId player,
    const std::uint64_t sequence, const eawr::sim::EntityId unit, tactical::CommandPayload payload) {
    return {{tick, player, sequence}, {unit}, std::move(payload)};
}

struct Run {
    std::vector<std::string> hashes;
    std::vector<math::Vec3> corvette;           // position after each tick
    std::vector<Fixed> frigate_shield;          // after each tick
    std::vector<tactical::Event> events;
    std::optional<tactical::TacticalSession> session;
};

[[nodiscard]] Run run(const std::vector<tactical::PlayerCommand>& commands, const std::vector<tactical::PlayerId>& humans,
    const std::size_t workers, const std::uint64_t ticks) {
    Run result;
    auto created = tactical::TacticalSession::create(
        setup(), {}, durability(), motion(), std::nullopt, {}, {}, abilities(humans));
    expect(static_cast<bool>(created), "the ability session is created");
    if (!created) return result;
    auto session = std::move(created).value();
    for (const auto& entry : commands) expect(static_cast<bool>(session.submit(entry)), "a command is accepted");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "a step succeeds");
        if (!stepped) break;
        result.hashes.push_back(stepped.value().state_sha256);
        for (const auto& event : stepped.value().snapshot->events()) result.events.push_back(event);
        for (const auto& unit : session.units()) {
            if (unit.entity_id == 1) result.corvette.push_back(unit.position);
        }
        const auto health = session.durability_state(2);
        result.frigate_shield.push_back(health ? health->shields : Fixed{});
    }
    result.session.emplace(std::move(session));
    return result;
}

void test_turbo_session() {
    // S-17 shape: a move at tick 30, TURBO at tick 60 (AB-24: the move plans again at once).
    const std::vector<tactical::PlayerCommand> plain{command(30, 1, 0, 1, tactical::MovePayload{at(3000, -1500)})};
    auto turbo_commands = plain;
    turbo_commands.push_back(command(60, 1, 1, 1, tactical::AbilityPayload{tactical::AbilityKind::turbo,
        tactical::AbilityAction::activate}));
    const auto slow = run(plain, {}, 1, 200);
    const auto fast = run(turbo_commands, {}, 1, 700);
    if (slow.corvette.size() < 200 || fast.corvette.size() < 700) return;
    expect(slow.corvette[60].x == fast.corvette[60].x, "TURBO leaves the ticks before it unchanged");
    const auto step = [](const Run& run, const std::size_t tick) {
        return Fixed::from_raw(run.corvette[tick].x.raw() - run.corvette[tick - 1].x.raw());
    };
    expect(step(slow, 199) == decimal("3.72"), "without TURBO the corvette cruises at 3.72");
    expect(step(fast, 199).raw() == 2 * step(slow, 199).raw(), "with TURBO it cruises at twice that");
    const auto state = fast.session->ability_state(1);
    expect(state && !state->slots[0].active && state->slots[0].ready_tick == 660 + 1500,
        "TURBO from tick 60 ran out at 660 and recharges to 2160");
    // AB-50: the snapshot's status.
    const auto snapshot = fast.session->snapshot();
    for (const auto& instance : snapshot->instances()) {
        if (instance.entity_id != 1) continue;
        expect(instance.abilities.size() == 1 && instance.abilities[0].kind == tactical::AbilityKind::turbo
                && !instance.abilities[0].active && !instance.abilities[0].ready
                && instance.abilities[0].total_frames == 1500 && instance.abilities[0].remaining_frames == 1460,
            "the snapshot shows TURBO recharging, 1460 of 1500 frames left");
    }
    // Every worker count and the recorded replay give the same hashes.
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run(turbo_commands, {}, workers, 700).hashes == fast.hashes,
            "TURBO hashes with " + std::to_string(workers) + " workers");
    }
    const auto replay = fast.session->record();
    auto bytes = tactical::write_replay(replay);
    auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
    expect(parsed && parsed.value() == replay, "an ability command survives the replay format");
    if (parsed) {
        auto again = tactical::TacticalSession::from_replay(
            parsed.value(), {}, durability(), motion(), std::nullopt, {}, {}, abilities({}));
        expect(static_cast<bool>(again), "the replay session is created");
        if (again) {
            const eawr::platform::ThreadWorkerAdapter executor(2);
            std::vector<std::string> hashes;
            while (again.value().completed_tick() < replay.final_tick_count) {
                hashes.push_back(again.value().step(executor).value().state_sha256);
            }
            expect(hashes == fast.hashes, "the replay reproduces every tick hash");
        }
    }
}

void test_defend_stand_in() {
    // AB-41 to AB-43: 400 damage at tick 500 makes the next window's rate 400 > 20; a non-human
    // owner's frigate switches DEFEND on at tick 510 and its shield recharges from tick 511 every
    // 9 frames (the S-15 shape, G-D7).
    const std::vector<tactical::PlayerCommand> hit{command(500, 2, 0, 2, tactical::DamagePayload{units(400)})};
    const auto ai = run(hit, {}, 1, 560);
    if (ai.frigate_shield.size() < 560) return;
    expect(ai.frigate_shield[500] == units(300), "the hit takes 400 of the 700 shield");
    std::vector<std::uint64_t> gains;
    // Index i holds the shield after the step that ran tick i; the frigate's own recharge phase may add
    // one gain before DEFEND, so only gains from tick 510 on are compared.
    for (std::size_t index = 510; index < 559; ++index) {
        if (ai.frigate_shield[index] > ai.frigate_shield[index - 1]) gains.push_back(index);
    }
    expect(gains.size() >= 3 && gains[0] == 511 && gains[1] == 520 && gains[2] == 529,
        "DEFEND recharges the shield at 511, 520 and 529");
    const auto state = ai.session->ability_state(2);
    expect(state && state->slots[0].active && state->slots[0].expires_tick == 510 + 450, "DEFEND runs until tick 960");
    // A human owner's frigate waits for autofire (the object script's human branch).
    const auto human = run(hit, {1}, 1, 560);
    const auto idle = human.session->ability_state(2);
    expect(idle && !idle->slots[0].active, "a human owner's DEFEND stays off without autofire");
    auto armed = hit;
    armed.push_back(command(10, 1, 0, 2, tactical::AbilityPayload{tactical::AbilityKind::defend,
        tactical::AbilityAction::autofire_on}));
    const auto autofire = run(armed, {1}, 1, 560);
    const auto fired = autofire.session->ability_state(2);
    expect(fired && fired->slots[0].active && fired->slots[0].autofire, "with autofire on it fires like the AI's");
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run(hit, {}, workers, 560).hashes == ai.hashes, "DEFEND hashes with " + std::to_string(workers) + " workers");
    }
}

void test_rejections() {
    const auto activate = [](const tactical::AbilityKind kind) {
        return tactical::AbilityPayload{kind, tactical::AbilityAction::activate};
    };
    const std::vector<tactical::PlayerCommand> commands{
        command(5, 1, 0, 1, activate(tactical::AbilityKind::defend)),  // the corvette has no DEFEND
        command(6, 1, 1, 1, activate(tactical::AbilityKind::turbo)),   // accepted
        command(7, 1, 2, 1, activate(tactical::AbilityKind::turbo)),   // already on
        command(8, 1, 3, 1, tactical::AbilityPayload{tactical::AbilityKind::turbo, tactical::AbilityAction::deactivate}),
        command(9, 1, 4, 1, activate(tactical::AbilityKind::turbo)),   // recharging (2 frames x 1500 / 600 = 5)
        command(9, 2, 0, 1, activate(tactical::AbilityKind::turbo)),   // not the owner
        command(9, 1, 5, 2, tactical::AbilityPayload{tactical::AbilityKind::defend, tactical::AbilityAction::autofire_on}),
        command(9, 1, 6, 1, tactical::AbilityPayload{tactical::AbilityKind::turbo, tactical::AbilityAction::autofire_on}),
    };
    const auto result = run(commands, {1}, 1, 20);
    std::vector<std::pair<tactical::EventKind, tactical::RejectReason>> seen;
    for (const auto& event : result.events) {
        if (event.order == tactical::OrderKind::ability) seen.emplace_back(event.kind, event.reason);
    }
    using K = tactical::EventKind;
    using R = tactical::RejectReason;
    const std::vector<std::pair<K, R>> expected{{K::order_rejected, R::ability_unavailable}, {K::order_accepted, R::none},
        {K::order_rejected, R::ability_unavailable}, {K::order_accepted, R::none}, {K::order_rejected, R::ability_unavailable},
        {K::order_accepted, R::none}, {K::order_rejected, R::ability_unavailable}, {K::order_rejected, R::unit_not_owned}};
    // In canonical order: tick, then player, then sequence.
    expect(seen == expected, "ability commands are accepted and rejected as AB-10 to AB-15 say");
    const auto state = result.session->ability_state(1);
    expect(state && !state->slots[0].active && state->slots[0].ready_tick == 8 + 5, "TURBO ended after 2 frames");
    const auto frigate = result.session->ability_state(2);
    expect(frigate && frigate->slots[0].autofire, "DEFEND is on autofire");
}

// #614: a squadron a hangar launches mid-battle locks its S-foils like a tick-zero one. The
// button's order goes to the squadron's container (AB-40); the launched container and craft
// enter with the ability off and ready (AB-10), so the order reaches the flying craft.
constexpr tactical::TypeId station_type = 20;
constexpr tactical::TypeId xwing_type = 21;
constexpr tactical::TypeId xwing_squadron = 22;

[[nodiscard]] tactical::MotionTable hangar_motion() {
    tactical::MotionTable table;
    tactical::CraftProfile craft;
    craft.type_id = xwing_type;
    craft.max_speed = decimal("5.4");
    craft.min_speed = decimal("1.8");
    craft.rate_of_turn = units(6);
    craft.lift = units(6);
    craft.thrust = decimal("0.2");
    craft.roll_rate = units(6);
    craft.bank_angle = units(70);
    craft.strafe_distance = units(200);
    table.squadrons.craft = {craft};
    table.squadrons.squadrons = {
        {xwing_squadron, {xwing_type, xwing_type}, {at(0, 0), at(-10, 10)}, units(1000), units(200), units(300), units(20)}};
    tactical::SpawnerProfile spawner;
    spawner.type_id = station_type;
    spawner.entries = {{xwing_squadron, 1, 0}};
    spawner.delay_frames = 150;
    spawner.bays = {{0, at(20, 0, -10), at(1, 0, -1)}};
    table.squadrons.spawners = {spawner};
    return table;
}

[[nodiscard]] tactical::DurabilityTable hangar_durability() {
    tactical::DurabilityTable table;
    table.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
    table.profiles = {tactical::DurabilityProfile{station_type, units(5000), std::nullopt, false, {}},
        tactical::DurabilityProfile{xwing_type, units(90), units(5), false, {}}};
    return table;
}

void test_launched_squadron_sfoils() {
    tactical::TacticalSetup setup;
    setup.seed = 614;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    // A tick-zero squadron (container 10, craft 11 and 12) beside the station; its hangar
    // launches one more (craft 13 and 14, container 15).
    setup.units = {{1, station_type, 1, at(0, 0), math::identity_quat(), {}},
        {10, xwing_squadron, 1, at(-500, 0), math::identity_quat(), {}},
        {11, xwing_type, 1, at(-500, 0), math::identity_quat(), {}}, {12, xwing_type, 1, at(-500, 0), math::identity_quat(), {}}};
    setup.squadrons = {{10, {11, 12}}};
    tactical::AbilityTable table;
    table.profiles = {unit_of(xwing_type, spoiler_lock())};
    const auto lock = [](const tactical::AbilityAction action) {
        return tactical::AbilityPayload{tactical::AbilityKind::spoiler_lock, action};
    };
    const auto foils = [](const tactical::TacticalSession& session, const eawr::sim::EntityId craft) {
        const auto state = session.ability_state(craft);
        return state && state->slots.size() == 1 && state->slots[0].active;
    };
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(
            setup, {}, hangar_durability(), hangar_motion(), std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "#614: the hangar session is created");
        if (!created) return;
        auto session = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const auto step_to = [&](const std::uint64_t tick) {
            while (session.completed_tick() < tick) {
                auto stepped = session.step(executor);
                expect(static_cast<bool>(stepped), "#614: a step succeeds");
                if (!stepped) return;
                hashes.push_back(stepped.value().state_sha256);
            }
        };
        const auto order = [&](const std::uint64_t tick, const std::uint64_t sequence, const eawr::sim::EntityId unit,
                               const tactical::AbilityAction action) {
            expect(static_cast<bool>(session.submit(command(tick, 1, sequence, unit, lock(action)))), "#614: the order is accepted");
        };
        step_to(60);
        expect(session.ability_state(13) && session.ability_state(14) && session.ability_state(15) == std::nullopt,
            "#614: the launched craft hold the ability, their container does not");
        expect(!foils(session, 11) && !foils(session, 13), "#614: the S-foils start off (AB-10)");
        // The starting and the launched squadron lock together, each by its container.
        order(61, 0, 10, tactical::AbilityAction::activate);
        order(61, 1, 15, tactical::AbilityAction::activate);
        step_to(70);
        expect(foils(session, 11) && foils(session, 12), "#614: the starting squadron's craft lock");
        expect(foils(session, 13) && foils(session, 14), "#614: the launched squadron's craft lock");
        // A snapshot shows the launched craft's ability as active, which is what the viewer draws.
        std::size_t shown = 0;
        for (const auto& instance : session.snapshot()->instances()) {
            if (instance.entity_id != 13 && instance.entity_id != 14) continue;
            for (const auto& ability : instance.abilities) {
                if (ability.kind == tactical::AbilityKind::spoiler_lock && ability.active) ++shown;
            }
        }
        expect(shown == 2, "#614: the snapshot shows both launched craft with SPOILER_LOCK on");
        // The order also reaches a launched craft that is ordered on its own.
        order(71, 0, 13, tactical::AbilityAction::deactivate);
        step_to(80);
        expect(!foils(session, 13) && foils(session, 14), "#614: a single launched craft unlocks alone");
        order(81, 0, 15, tactical::AbilityAction::deactivate);
        order(81, 1, 10, tactical::AbilityAction::deactivate);
        step_to(90);
        expect(!foils(session, 11) && !foils(session, 12) && !foils(session, 13) && !foils(session, 14),
            "#614: both squadrons unlock");
        if (baseline.empty()) {
            baseline = hashes;
        } else {
            expect(hashes == baseline, "#614: the hangar session hashes with " + std::to_string(workers) + " workers");
        }
    }
}

} // namespace

int main() {
    test_names_and_validation();
    test_timers();
    test_multipliers();
    test_turbo_session();
    test_defend_stand_in();
    test_rejections();
    test_launched_squadron_sfoils();
    if (failures != 0) {
        std::cerr << failures << " ability contract failure(s)\n";
        return 1;
    }
    std::cout << "ability contracts passed\n";
    return 0;
}
