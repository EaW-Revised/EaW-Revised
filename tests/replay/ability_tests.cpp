#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"

#include <algorithm>
#include <cstdlib>
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
    expect(tactical::ability_kind("invulnerability") == tactical::AbilityKind::invulnerability,
        "WHE-22: Falcon mode has an ordinary ability kind");
    expect(tactical::ability_kind("DEFEND") == tactical::AbilityKind::defend, "DEFEND");
    expect(tactical::ability_kind("power_to_weapons") == tactical::AbilityKind::power_to_weapons,
        "the Acclamator's lower-case spelling");
    expect(tactical::ability_kind("Spoiler_Lock") == tactical::AbilityKind::spoiler_lock, "any case");
    expect(tactical::ability_kind("HUNT") == tactical::AbilityKind::hunt, "WAB-50: HUNT is modelled");
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
    auto defaults = table;
    defaults.autofire_defaults = {1};
    expect(static_cast<bool>(tactical::validate_abilities(defaults)), "a human creation preference is valid");
    defaults.autofire_defaults = {2};
    expect(!tactical::validate_abilities(defaults), "an AI owner is not a local profile owner");
    defaults.autofire_defaults = {1, 1};
    expect(!tactical::validate_abilities(defaults), "duplicate creation preferences are rejected");
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
    table.autofire_defaults = humans;
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

void test_expiry_snapshot_keeps_timed_duration() {
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto table = abilities({1});
        table.profiles.front().abilities.front().expiration_frames = 3;
        table.profiles.front().abilities.front().recharge_frames = 7;
        auto created = tactical::TacticalSession::create(
            setup(), {}, durability(), motion(), std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "the expiry boundary session is created");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(command(0, 1, 0, 1,
            tactical::AbilityPayload{tactical::AbilityKind::turbo, tactical::AbilityAction::activate}))),
            "the short timed ability is accepted");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint32_t completed = 1; completed <= 4; ++completed) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "the expiry boundary step succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto& instances = stepped.value().snapshot->instances();
            const auto instance = std::find_if(instances.begin(), instances.end(), [](const auto& value) {
                return value.entity_id == 1;
            });
            expect(instance != instances.end() && instance->abilities.size() == 1,
                "the timed ability is published");
            if (instance == instances.end() || instance->abilities.size() != 1) continue;
            const auto& status = instance->abilities.front();
            if (completed <= 3) {
                expect(status.active && status.total_frames == 3 && status.remaining_frames == 3 - completed,
                    "AB-05: every active snapshot retains its duration, including zero remaining");
            } else {
                expect(!status.active && status.total_frames == 7 && status.remaining_frames == 6,
                    "the next snapshot publishes the recharge countdown");
            }
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "the expiry boundary hashes agree for 1/2/4/8 workers");
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
    // AB-45: a human owner's supported ability starts armed, as a fresh retail profile does.
    const auto human_default = run(hit, {1}, 1, 560);
    const auto armed_default = human_default.session->ability_state(2);
    expect(armed_default && armed_default->slots[0].active && armed_default->slots[0].autofire,
        "a human owner's default DEFEND autofire needs no ability command");
    auto manual = hit;
    manual.push_back(command(10, 1, 0, 2, tactical::AbilityPayload{tactical::AbilityKind::defend,
        tactical::AbilityAction::autofire_off}));
    const auto human = run(manual, {1}, 1, 560);
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
        expect(run(hit, {1}, workers, 560).hashes == human_default.hashes,
            "human default autofire hashes with " + std::to_string(workers) + " workers");
    }
}

void test_autofire_rules() {
    const tactical::UnitAbilityProfile mixed{9, {turbo(), defend()}, true};
    const auto enabled = tactical::initial_abilities(mixed, true);
    expect(!enabled.slots[0].autofire && enabled.slots[1].autofire,
        "AB-45: only supported abilities start armed, including a secondary ability");
    expect(!enabled.slots[0].active && !enabled.slots[1].active,
        "AB-40: arming never activates an ability");
    const auto disabled = tactical::initial_abilities(mixed, false);
    expect(!disabled.slots[1].autofire, "AB-45: the disabled creation preference is retained");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const auto idle = run({}, {1}, workers, 31);
        const auto ready = idle.session->ability_state(2);
        expect(ready && ready->slots[0].autofire && !ready->slots[0].active,
            "AB-41: an armed idle frigate needs damage, not merely an enemy");
        for (const auto damage : {20, 21}) {
            const auto result = run({command(10, 2, 0, 2, tactical::DamagePayload{units(damage)})}, {1}, workers, 31);
            const auto state = result.session->ability_state(2);
            expect(state && state->slots[0].active == (damage > 20),
                "AB-41: the damage threshold is strictly greater than twenty");
        }
        const auto depleted = run({command(10, 2, 0, 2, tactical::DamagePayload{units(700)})}, {1}, workers, 31);
        const auto state = depleted.session->ability_state(2);
        expect(state && state->slots[0].autofire && !state->slots[0].active,
            "AB-14: autofire cannot bypass the shield depletion gate");
        const auto cooldown = run({command(10, 2, 0, 2, tactical::DamagePayload{units(21)}),
            command(500, 2, 1, 2, tactical::DamagePayload{units(21)})}, {1}, workers, 541);
        const auto cooling = cooldown.session->ability_state(2);
        expect(cooling && !cooling->slots[0].active && cooling->slots[0].ready_tick == 2280,
            "AB-13: damage while recharging does not retrigger DEFEND");
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

void test_created_squadron_autofire() {
    // AB-45: a supported team ability is created for the hangar's container and craft.
    // This synthetic squadron uses the hangar fixture's IDs and motion, without weapons.
    tactical::TacticalSetup battle;
    battle.seed = 1054;
    battle.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    battle.units = {{1, station_type, 1, at(0, 0), math::identity_quat(), {}},
        {10, xwing_squadron, 1, at(-500, 0), math::identity_quat(), {}},
        {11, xwing_type, 1, at(-500, 0), math::identity_quat(), {}},
        {12, xwing_type, 1, at(-500, 0), math::identity_quat(), {}}};
    battle.squadrons = {{10, {11, 12}}};
    tactical::AbilityProfile ion;
    ion.kind = tactical::AbilityKind::ion_cannon_shot;
    ion.supports_autofire = true;
    auto team = ion;
    team.team = true;
    tactical::AbilityTable table;
    table.profiles = {unit_of(xwing_type, ion), unit_of(xwing_squadron, team)};
    table.humans = {1};
    table.autofire_defaults = {1};
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(battle, {}, hangar_durability(), hangar_motion(),
            std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "AB-45: the supported hangar session is created");
        if (!created) return;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(command(0, 1, 0, 10,
            tactical::AbilityPayload{tactical::AbilityKind::ion_cannon_shot, tactical::AbilityAction::autofire_off}))),
            "the starting container's autofire can be switched off");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::size_t tick = 0; tick < 60; ++tick) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "the supported hangar session steps");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
        }
        const auto original = session.ability_state(10);
        expect(original && !original->slots[0].autofire, "an existing toggle is not reapplied every tick");
        for (const auto id : {13U, 14U, 15U}) {
            const auto spawned = session.ability_state(id);
            expect(spawned && spawned->slots[0].autofire && !spawned->slots[0].active,
                "AB-45: newly launched craft and their container inherit the creation preference");
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "AB-45: launched defaults are deterministic for 1/2/4/8 workers");
    }
}

struct SpecialProbe final : tactical::SpecialAbilityHandler {
    bool is_ready{true}, mode_ok{true}, target_ok{true}, is_available{true}, succeeds{true};
    std::vector<std::size_t> applied, serviced, terminated;
    std::vector<eawr::sim::EntityId> removed;
    std::size_t target_checks{};
    bool context_valid{true}, schedule_valid{true};
    bool ready(std::size_t, const tactical::SpecialActivationContext&) const override { return is_ready; }
    bool appropriate_mode(std::size_t, tactical::SpecialAbilityMode) const override { return mode_ok; }
    bool appropriate_target(std::size_t, const tactical::SpecialActivationContext&) const override { return target_ok; }
    bool available(std::size_t, const tactical::SpecialActivationContext&) const override { return is_available; }
    bool apply(const std::size_t slot, tactical::SpecialAbilitySlot& state, const tactical::SpecialActivationContext& context) override {
        applied.push_back(slot);
        context_valid = context_valid && state.context == context;
        return succeeds && slot != 0;
    }
    void service(const std::size_t slot, tactical::SpecialAbilitySlot& state, const std::uint64_t frame) override {
        serviced.push_back(slot);
        schedule_valid = schedule_valid && state.next_service_frame == frame + 1;
    }
    bool target_live(const eawr::sim::EntityId target) const override { return target != 99; }
    void remove_effect(std::size_t, const eawr::sim::EntityId target) override { removed.push_back(target); ++target_checks; }
    void terminate(const std::size_t slot) override { terminated.push_back(slot); }
};

void test_hero_damage_modes() {
    tactical::AbilityProfile falcon;
    falcon.kind = tactical::AbilityKind::invulnerability;
    falcon.expiration_frames = 180;
    falcon.recharge_frames = 1800;
    falcon.modifiers.take_damage = Fixed{};
    falcon.supports_autofire = true;
    auto profile = unit_of(corvette_type, falcon);
    auto state = tactical::initial_abilities(profile);
    expect(tactical::ability_multiplier(profile, state, tactical::AbilityModifier::take_damage) == units(1),
        "WHE-22: inactive Falcon takes ordinary damage");
    expect(tactical::activate_ability(falcon, state.slots[0], {}, 10).changed,
        "WHE-22: Falcon activation succeeds");
    expect(state.slots[0].expires_tick == 190
        && tactical::ability_multiplier(profile, state, tactical::AbilityModifier::take_damage) == Fixed{},
        "WHE-22: active Falcon authors six seconds of zero take damage");
    static_cast<void>(tactical::expire_abilities(profile, state, 189));
    expect(state.slots[0].active, "WHE-22: Falcon stays active until its expiry frame");
    static_cast<void>(tactical::expire_abilities(profile, state, 190));
    expect(!state.slots[0].active && state.slots[0].ready_tick == 1990
        && tactical::ability_multiplier(profile, state, tactical::AbilityModifier::take_damage) == units(1),
        "WHE-22: expiry restores ordinary damage and the generic sixty-second cooldown");
    expect(!tactical::ability_ready(falcon, state.slots[0], {}, 1989)
        && tactical::ability_ready(falcon, state.slots[0], {}, 1990), "WHE-22: exact cooldown boundary");

    auto table = durability();
    auto hull = table.profiles.front();
    hull.max_shields = Fixed{};
    // WHE-51: direct, area, environmental and privileged script delivery all take the mode.
    for (const unsigned kind : {0U, 1U, 2U, 3U}) {
        tactical::Hit hit;
        hit.amount = units(20);
        hit.projectile = kind < 2;
        hit.area = kind == 1;
        hit.kind = kind == 2 ? tactical::HitKind::asteroid : tactical::HitKind::ordinary;
        hit.take_damage_multiplier = Fixed{};
        auto health = tactical::full_durability(hull);
        const auto immune = tactical::apply_hit(hull, *table.damage, health, hit, 0);
        expect(immune && health.hull == hull.max_hull && !immune.value().cancelled,
            "WHE-51: zero mode prevents damage without canceling a projectile blast");
        hit.bypass_take_damage_mode = true;
        const auto bypassed = tactical::apply_hit(hull, *table.damage, health, hit, 1);
        expect(bypassed && health.hull == Fixed::from_raw(hull.max_hull.raw() - units(20).raw()),
            "WHE-51: only the explicit delivery bypass skips the take mode");
    }
    tactical::Hit hit;
    hit.amount = units(20);
    hit.projectile = true;
    hit.cause_damage_multiplier = decimal("2.5");
    hit.take_damage_multiplier = decimal("0.5");
    auto health = tactical::full_durability(hull);
    const auto boosted = tactical::apply_hit(hull, *table.damage, health, hit, 0);
    expect(boosted && health.hull == Fixed::from_raw(hull.max_hull.raw() - units(25).raw()),
        "WHE-51: take and cause are multiplicative modes, separate from fractional bonuses");

    std::vector<std::string> expiry_baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto raw_health = durability();
        raw_health.damage.reset();
        tactical::AbilityTable modes;
        modes.profiles = {unit_of(corvette_type, falcon)};
        modes.humans = {1};
        auto created = tactical::TacticalSession::create(setup(), {}, raw_health, {}, std::nullopt, {}, {}, modes);
        expect(static_cast<bool>(created), "WHE-52: raw-damage Falcon session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(command(0, 1, 0, 1,
            tactical::AbilityPayload{falcon.kind, tactical::AbilityAction::activate}))), "WHE-52: raw Falcon activates");
        for (const auto frame : {1U, 179U, 180U}) {
            expect(static_cast<bool>(session.submit(command(frame, 1, frame, 1,
                tactical::DamagePayload{units(10), tactical::hull_target}))), "WHE-52: raw scripted hit submits");
        }
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (unsigned frame = 0; frame <= 180; ++frame) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "WHE-52: raw Falcon step succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
        }
        const auto final = session.durability_state(1);
        const auto ability = session.ability_state(1);
        expect(final && final->hull == units(3590) && ability && !ability->slots[0].active
            && ability->slots[0].ready_tick == 1980,
            "WHE-51/52: raw script damage is blocked until exact expiry, then restored with full recharge");
        if (expiry_baseline.empty()) expiry_baseline = hashes;
        else expect(hashes == expiry_baseline, "WHE-52: raw expiry hashes agree on 1/2/4/8 workers");
    }

    // An in-flight shot observes switches at delivery, rather than retaining launch modes.
    std::vector<std::string> baseline;
    for (const unsigned scenario : {0U, 1U, 2U, 3U}) {
        baseline.clear();
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto health_table = durability();
            for (auto& row : health_table.profiles) { row.max_shields = Fixed{}; row.shield_refresh = Fixed{}; }
            tactical::AbilityProfile assault;
            assault.kind = tactical::AbilityKind::power_to_weapons;
            assault.expiration_frames = 21;
            assault.recharge_frames = 50;
            assault.modifiers.cause_damage = units(2);
            tactical::AbilityTable modes;
            modes.profiles = {unit_of(corvette_type, assault), unit_of(frigate_type, falcon)};
            modes.humans = {1, 2};
            tactical::CombatTable combat;
            tactical::CombatProfile shooter;
            shooter.type_id = corvette_type;
            shooter.category_bits = 1;
            shooter.max_attack_distance = units(700);
            tactical::WeaponProfile weapon;
            weapon.range = units(700);
            weapon.min_recharge_hundredths = 0;
            weapon.max_recharge_hundredths = 0;
            weapon.pulse_count = 1;
            weapon.cone_width = units(175); weapon.cone_height = units(160);
            weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
            weapon.shot = tactical::ShotProfile{units(10), 0, units(25), units(700), true, true, {}};
            shooter.weapons = {weapon};
            tactical::CombatProfile target;
            target.type_id = frigate_type; target.category_bits = 2;
            target.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
            combat.profiles = {shooter, target};
            auto initial = setup();
            initial.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(300, 0), math::identity_quat(), {}}};
            auto created = tactical::TacticalSession::create(initial,
                std::vector<tactical::SensorProfile>{{corvette_type, units(1000)}, {frigate_type, units(1000)}},
                health_table, {}, std::nullopt, combat, {}, modes);
            expect(static_cast<bool>(created), "WHE-51: damage-mode projectile session creates");
            if (!created) continue;
            auto session = std::move(created).value();
            expect(static_cast<bool>(session.submit(command(0, 1, 0, 1,
                tactical::AttackPayload{2}))), "WHE-51: object weapon receives an attack order");
            expect(static_cast<bool>(session.submit(command(1, 1, 1, 1,
                tactical::AbilityPayload{assault.kind, tactical::AbilityAction::activate}))), "WHE-51: Assault submits");
            expect(static_cast<bool>(session.submit(command(1, 1, 2, 1,
                tactical::StopPayload{}))), "WHE-51: stop leaves just the first shot in flight");
            if (scenario == 1) expect(static_cast<bool>(session.submit(command(2, 1, 3, 1,
                tactical::AbilityPayload{assault.kind, tactical::AbilityAction::deactivate}))), "WHE-51: early switch submits");
            if (scenario == 2) {
                expect(static_cast<bool>(session.submit(command(1, 2, 0, 2,
                    tactical::AbilityPayload{falcon.kind, tactical::AbilityAction::activate}))), "WHE-22: Falcon submits");
                expect(static_cast<bool>(session.submit(command(2, 2, 1, 2,
                    tactical::DamagePayload{units(100), tactical::hull_target}))), "WHE-51: privileged script hit submits");
            }
            if (scenario == 3) expect(static_cast<bool>(session.submit(command(2, 1, 3, 1,
                tactical::DamagePayload{units(100000), tactical::hull_target}))), "WHE-51: source removal submits");
            std::vector<std::string> hashes;
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            unsigned hits = 0;
            for (unsigned tick = 0; tick < 40; ++tick) {
                auto stepped = session.step(executor);
                expect(static_cast<bool>(stepped), "WHE-51: projectile step succeeds");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                for (const auto& event : stepped.value().snapshot->combat_events())
                    if (event.kind == tactical::CombatEventKind::projectile_hit) ++hits;
            }
            expect(hits == 1, "WHE-51: the fixture delivers exactly one in-flight hit");
            const auto final = session.durability_state(2);
            expect(final && final->hull == units(3600 - (scenario == 0 ? 20 : scenario == 2 ? 0 : 10)),
                "WHE-51: current Assault/Falcon modes affect projectile and script damage at delivery");
            if (baseline.empty()) baseline = hashes;
            else expect(hashes == baseline, "WHE-51: damage-mode hashes agree on 1/2/4/8 workers");
            const auto recorded = session.record();
            const auto bytes = tactical::write_replay(recorded);
            const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == recorded, "WHE-22: invulnerability kind round-trips through replay commands");
        }
    }
}

void test_impact_shooter_bonus() {
    // WPR-51/WCC-44, EUS-16: instance damage, live shooter bonus and victim defense
    // are independent. Exercise both weapon fire and an explicit stationary launch.
    for (const unsigned route : {0U, 1U, 2U}) for (const unsigned scenario : {0U, 1U, 2U, 3U, 4U, 5U, 6U}) {
        const bool spawned = route != 0, delayed = route == 2;
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; row.hardpoints.clear(); }
            tactical::CombatProfile shooter;
            shooter.type_id = corvette_type; shooter.max_attack_distance = units(700);
            tactical::WeaponProfile weapon;
            weapon.range = units(700); weapon.pulse_count = 1;
            weapon.cone_width = units(175); weapon.cone_height = units(160);
            weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
            weapon.shot = tactical::ShotProfile{units(10), 0, units(25), units(700), true, true, {}};
            if (!spawned) shooter.weapons = {weapon};
            tactical::CombatProfile target;
            target.type_id = frigate_type;
            target.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
            tactical::CombatProfile provider; provider.type_id = plain_type;
            tactical::CombatTable combat; combat.profiles = {shooter, target, provider};
            tactical::AbilityTable abilities;
            if (spawned) {
                tactical::AbilityProfile bomb;
                bomb.kind = tactical::AbilityKind::harmonic_bomb; bomb.recharge_frames = 100;
                bomb.spawned = tactical::SpawnedAbilityProfile{};
                bomb.spawned->type = 99; bomb.spawned->countdown_frames = 5;
                bomb.spawned->damage_type = 0;
                bomb.spawned->blast.damage = units(10); bomb.spawned->blast.radius = units(400);
                bomb.spawned->blast.max_delay = delayed ? decimal("0.2") : Fixed{};
                abilities.profiles = {unit_of(corvette_type, bomb)};
            }
            tactical::EconomyRules economy;
            tactical::CommandBonusProfile bonus;
            bonus.type = plain_type; bonus.bonus.applicable = {corvette_type};
            bonus.bonus.percentages[1] = decimal("0.5");
            tactical::CommandBonusProfile defense;
            defense.type = plain_type; defense.slot = 1; defense.bonus.applicable = {frigate_type};
            defense.bonus.percentages[4] = decimal("0.25");
            economy.command_bonuses = {bonus, defense};
            auto initial = setup();
            initial.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(300, 0), math::identity_quat(), {}}};
            if (scenario != 0 && scenario != 4)
                initial.units.push_back({3, plain_type, 1, at(3000, 0), math::identity_quat(), {}});
            if (scenario == 5 || scenario == 6)
                initial.units.push_back({4, plain_type, 2, at(3000, 1000), math::identity_quat(), {}});
            auto created = tactical::TacticalSession::create(initial,
                std::vector<tactical::SensorProfile>{{corvette_type, units(1000)}, {frigate_type, units(1000)}},
                health, {}, std::nullopt, combat, {}, abilities, economy);
            expect(static_cast<bool>(created), "EUS-16: impact bonus fixture creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            if (spawned) expect(static_cast<bool>(world.submit(command(0, 1, 0, 1,
                tactical::AbilityPayload{tactical::AbilityKind::harmonic_bomb, tactical::AbilityAction::activate}))),
                "EUS-16: explicit launch submits");
            else {
                expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, tactical::AttackPayload{2}))),
                    "EUS-16: ordinary launch submits");
                expect(static_cast<bool>(world.submit(command(1, 1, 1, 1, tactical::StopPayload{}))),
                    "EUS-16: fixture stops after one shot");
            }
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            unsigned hits = 0;
            for (unsigned frame = 0; frame < 30; ++frame) {
                if (frame == 2) {
                    if (scenario == 2 || scenario == 3 || scenario == 5)
                        expect(static_cast<bool>(world.stage_remove(scenario == 2 ? 3 : scenario == 3 ? 1 : 4)),
                            "EUS-16: source/bonus/defense removal before contact succeeds");
                    if (scenario == 4) expect(static_cast<bool>(world.stage_spawn(
                        {3, plain_type, 1, at(3000, 0), math::identity_quat(), {}})),
                        "EUS-16: new damage bonus before contact succeeds");
                }
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "EUS-16: impact bonus step succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
                if (frame == 0 && !spawned) {
                    const auto shots = step.value().snapshot->projectiles();
                    expect(shots.size() == 1 && shots.front().damage == units(10),
                        "EUS-16: projectile stores base instance damage without baking shooter bonus");
                }
                for (const auto& event : step.value().snapshot->combat_events())
                    if (event.kind == tactical::CombatEventKind::projectile_hit) ++hits;
            }
            const auto final = world.durability_state(2);
            // EUS-15/WAD-26: queued raw damage drops shooter modifiers; recipient defense
            // still resolves at delivery. EUS-16 keeps the current bonus for immediate hits.
            const bool boosted = !delayed && (scenario == 1 || scenario == 4 || scenario == 5 || scenario == 6);
            const auto amount = scenario == 6 ? decimal(delayed ? "7.5" : "11.25") : units(boosted ? 15 : 10);
            expect(final && final->hull == Fixed::from_raw(units(3600).raw() - amount.raw()) && (spawned || hits == 1),
                delayed ? "EUS-15/WAD-26: queued amount drops source bonus and reads current defense"
                    : "EUS-16: contact uses current bonus once, removed shooter contributes none, current defense survives");
            if (reference.empty()) reference = hashes;
            else expect(reference == hashes, "EUS-16: impact bonus hashes agree at 1/2/4/8 workers");
        }
    }
}

void test_concentrate_damage() {
    for (const unsigned scenario : {0U, 1U, 2U, 3U, 4U, 5U}) {
        std::vector<std::string> baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
            tactical::AbilityProfile ordinary;
            ordinary.kind = tactical::AbilityKind::concentrate_fire;
            ordinary.expiration_frames = 100;
            ordinary.recharge_frames = 20;
            ordinary.effective_radius = units(6000);
            ordinary.gui_activated_ability_name = "focus";
            auto hero = unit_of(plain_type, ordinary);
            tactical::SpecialAbilityProfile special;
            special.name = "focus"; special.kind = tactical::SpecialAbilityKind::concentrate_fire;
            special.style = tactical::SpecialActivationStyle::user_input; special.service_interval = 1;
            special.target_damage_increase = decimal("0.5"); special.filter.applicable_categories = 2;
            hero.special = {special};
            tactical::AbilityTable abilities;
            abilities.profiles = {hero};
            tactical::CombatTable combat;
            tactical::CombatProfile shooter;
            shooter.type_id = corvette_type; shooter.category_bits = 1; shooter.max_attack_distance = units(700);
            tactical::WeaponProfile weapon;
            weapon.range = units(700); weapon.pulse_count = 1;
            weapon.cone_width = units(175); weapon.cone_height = units(160);
            weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
            weapon.shot = tactical::ShotProfile{units(10), 0, units(25), units(700), true, true, {}};
            shooter.weapons = {weapon};
            tactical::CombatProfile target;
            target.type_id = frigate_type; target.category_bits = 2;
            target.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
            tactical::CombatProfile source; source.type_id = plain_type; source.category_bits = 4;
            combat.profiles = {shooter, target, source};
            auto initial = setup();
            initial.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(300, 0), math::identity_quat(), {}},
                {3, plain_type, 1, at(3000, 0), math::identity_quat(), {}}};
            tactical::EconomyRules economy;
            if (scenario >= 4) {
                tactical::CommandBonusProfile bonus;
                bonus.type = frigate_type; bonus.apply_to_self = true;
                bonus.bonus.applicable = {frigate_type};
                bonus.bonus.stacking_category = scenario == 4 ? 0 : 1;
                bonus.bonus.percentages[4] = decimal("0.25");
                economy.command_bonuses = {bonus};
            }
            auto created = tactical::TacticalSession::create(initial,
                std::vector<tactical::SensorProfile>{{corvette_type, units(1000)}, {frigate_type, units(1000)}},
                health, {}, std::nullopt, combat, {}, abilities, economy);
            expect(static_cast<bool>(created), "WHE-25: concentrate-fire damage session creates");
            if (!created) continue;
            auto session = std::move(created).value();
            expect(static_cast<bool>(session.submit(command(0, 1, 0, 1, tactical::AttackPayload{2}))), "single-shot fixture attack submits");
            if (scenario != 0) expect(static_cast<bool>(session.submit(command(1, 1, 1, 3,
                tactical::AbilityPayload{ordinary.kind, tactical::AbilityAction::activate, 2}))), "concentrate fire locks target");
            expect(static_cast<bool>(session.submit(command(1, 1, 2, 1, tactical::StopPayload{}))), "single-shot fixture stops firing");
            if (scenario == 2) expect(static_cast<bool>(session.submit(command(2, 1, 3, 3,
                tactical::AbilityPayload{ordinary.kind, tactical::AbilityAction::deactivate}))), "release submits before impact");
            if (scenario == 3) expect(static_cast<bool>(session.submit(command(2, 1, 3, 3,
                tactical::DamagePayload{units(100000), tactical::hull_target}))), "source death submits before impact");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            unsigned hits = 0;
            for (unsigned frame = 0; frame < 30; ++frame) {
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), "concentrate-fire damage step succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
                for (const auto& event : step.value().snapshot->combat_events()) if (event.kind == tactical::CombatEventKind::projectile_hit) ++hits;
            }
            const auto final = session.durability_state(2);
            const auto expected = scenario == 1 ? decimal("15") : scenario == 4 ? decimal("7.5")
                : scenario == 5 ? decimal("12.5") : units(10);
            expect(hits == 1 && final && final->hull == Fixed::from_raw(units(3600).raw() - expected.raw()),
                "WHE-25/55: target defense applies at delivery; release/death restore; same-category positive wins, distinct categories add");
            if (baseline.empty()) baseline = hashes;
            else expect(hashes == baseline, "concentrate-fire damage hashes equal at 1/2/4/8 workers");
            const auto record = session.record();
            const auto encoded = tactical::write_replay(record);
            const auto parsed = encoded ? tactical::parse_replay(encoded.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == record, "concentrate-fire targeted commands round-trip through the existing opcode");
        }
    }
}

void test_concentrate_fire() {
    using K = tactical::AbilityKind;
    tactical::AbilityProfile ordinary;
    ordinary.kind = K::concentrate_fire;
    ordinary.effective_radius = units(6000);
    ordinary.expiration_frames = 3;
    ordinary.recharge_frames = 6;
    ordinary.gui_activated_ability_name = "focus";
    tactical::SpecialAbilityProfile nested;
    nested.name = "focus";
    nested.kind = tactical::SpecialAbilityKind::concentrate_fire;
    nested.style = tactical::SpecialActivationStyle::user_input;
    nested.service_interval = 1;
    nested.filter.applicable_categories = 1;
    nested.target_damage_increase = decimal("0.5");
    auto hero = unit_of(corvette_type, ordinary);
    hero.special = {nested};
    tactical::AbilityTable abilities;
    abilities.profiles = {hero};
    tactical::CombatTable combat;
    for (const auto type : {corvette_type, frigate_type, plain_type, xwing_type}) {
        tactical::CombatProfile item;
        item.type_id = type;
        item.category_bits = 1;
        combat.profiles.push_back(item);
    }
    std::sort(combat.profiles.begin(), combat.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    auto battle = setup();
    battle.players.push_back({3, 1, 1, tactical::player_flag_commandable});
    battle.units = {
        {1, corvette_type, 1, at(-20000, 0), math::identity_quat(), {}},
        {2, frigate_type, 1, at(10000, 100), math::identity_quat(), {}},
        {3, plain_type, 2, at(10000, 0), math::identity_quat(), {}},
        {4, frigate_type, 3, at(10000, 200), math::identity_quat(), {}},
        {5, frigate_type, 1, at(-19900, 0), math::identity_quat(), {}},
        {6, frigate_type, 1, at(16001, 0), math::identity_quat(), {}},
        {10, xwing_squadron, 1, at(10000, 300), math::identity_quat(), {}},
        {11, xwing_type, 1, at(10000, 300), math::identity_quat(), {}},
        {12, xwing_type, 1, at(10000, 310), math::identity_quat(), {}},
    };
    battle.squadrons = {{10, {11, 12}}};
    // Spread a large unrelated census across far-away cells: recruitment work stays local.
    for (unsigned i = 0; i < 512; ++i) battle.units.push_back({100 + i, frigate_type, 1,
        at(100000 + 1000 * i, 100000), math::identity_quat(), {}});
    auto movement = motion();
    movement.squadrons = hangar_motion().squadrons;
    movement.squadrons.spawners.clear();
    auto health = durability();
    health.profiles.push_back({xwing_type, units(90), units(5), false, {}});
    std::sort(health.profiles.begin(), health.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(battle, {}, health, movement, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(created), "WHE-24: concentrate-fire session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        const auto activate = tactical::AbilityPayload{K::concentrate_fire, tactical::AbilityAction::activate, 3};
        expect(static_cast<bool>(session.submit(command(0, 1, 0, 1, activate))), "targeted concentrate command submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 5; ++tick) {
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "concentrate-fire step succeeds");
            if (!step) break;
            hashes.push_back(step.value().state_sha256);
            const auto state = session.ability_state(1);
            if (tick == 0) {
                expect(state && state->slots[0].active && state->slots[0].target == 3
                    && state->concentrate_recruits == std::vector<eawr::sim::EntityId>{2, 10},
                    "WHE-24: target-centred own units; source/allied owner/out-of-range excluded; craft promote once");
                const auto work = session.tick_work();
                expect(work.concentrate_queries == 1 && work.concentrate_candidates < 30 && work.concentrate_batches == 1,
                    "one bounded spatial recruitment query and one attack batch, independent of remote census");
                const auto live_units = session.units();
                const auto ordered = std::find_if(live_units.begin(), live_units.end(), [](const auto& unit) { return unit.entity_id == 2; });
                expect(ordered != live_units.end() && ordered->order.kind == tactical::OrderKind::attack && ordered->order.target == 3,
                    "recruited ships use the ordinary attack/approach path");
            } else {
                expect(session.tick_work().concentrate_queries == 0, "recruitment is activation-only");
                if (tick >= 3) expect(state && !state->slots[0].active && state->slots[0].target == 0
                    && state->concentrate_recruits.empty() && state->special.slots[0].targets.empty(),
                    "WHE-25: expiry releases the target and recruitment ledgers");
            }
        }
        if (workers == 1) baseline = hashes;
        else expect(hashes == baseline, "concentrate state/events equal at 1/2/4/8 workers");
    }
}

void test_concentrate_sparse_lifecycle() {
    for (const unsigned scenario : {0U, 1U, 2U}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, plain_type, 1, at(-2200, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(0, 0), math::identity_quat(), {}},
                {3, corvette_type, 1, at(100, 600), math::identity_quat(), {}},
                {4, 99, 2, at(0, 0), math::identity_quat(), {}}};
            tactical::AbilityProfile ordinary;
            ordinary.kind = tactical::AbilityKind::concentrate_fire;
            ordinary.expiration_frames = 20; ordinary.recharge_frames = 20;
            ordinary.effective_radius = units(6000); ordinary.gui_activated_ability_name = "focus";
            auto source = unit_of(plain_type, ordinary);
            tactical::SpecialAbilityProfile nested;
            nested.name = "focus"; nested.kind = tactical::SpecialAbilityKind::concentrate_fire;
            nested.style = tactical::SpecialActivationStyle::user_input; nested.service_interval = 1;
            nested.target_damage_increase = decimal("0.5"); nested.filter.applicable_categories = 1;
            source.special = {nested};
            tactical::AbilityTable abilities; abilities.profiles = {source};
            tactical::CombatTable combat;
            for (const auto type : {corvette_type, frigate_type, plain_type}) {
                tactical::CombatProfile profile; profile.type_id = type; profile.category_bits = 1;
                combat.profiles.push_back(profile);
            }
            auto health = durability();
            for (auto& profile : health.profiles) { profile.max_shields = {}; profile.shield_refresh = {}; }
            auto movement = motion();
            if (scenario == 0) {
                movement.avoidance = tactical::AvoidanceRules{units(24), decimal("0.2"), units(100), decimal("0.8"), units(15),
                    decimal("0.66"), decimal("1.2"), decimal("0.25"), decimal("1.7"), decimal("0.5"), decimal("0.5"), 3500, 6, 90, 45, units(50)};
                tactical::Footprint victim; victim.type_id = frigate_type; victim.layer = tactical::SpaceLayer::frigate;
                victim.radius = units(10); victim.asteroid_damage = true; victim.locomotor = false;
                tactical::Footprint field; field.type_id = 99; field.layer = tactical::SpaceLayer::static_object;
                field.asteroid_field = true; field.obstacle = true; field.radius = units(100);
                movement.footprints = {victim, field};
                health.damage->asteroid_damage = units(20); health.damage->asteroid_rate = units(1);
            }
            tactical::EconomyRules economy;
            economy.players = {{1, units(100), 25, false, {}, 1, 3}};
            tactical::BuildOption option;
            option.type = scenario == 0 ? 90 : corvette_type;
            option.kind = scenario == 0 ? tactical::BuildKind::upgrade : tactical::BuildKind::unit;
            option.queue = scenario == 0 ? tactical::BuildQueue::upgrades : tactical::BuildQueue::units;
            option.price = units(10);
            option.build_frames = 2; option.ai_build_frames = 2; option.available = true;
            economy.menus = {{plain_type, 1, {option}}};
            economy.footprints = {{corvette_type, std::nullopt, {}}};
            if (scenario == 0) {
                tactical::UpgradeBonus bonus; bonus.applicable = {corvette_type}; bonus.percentages[5] = decimal("0.25");
                economy.upgrades = {{90, false, false, 0, {bonus}}};
            }
            auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt,
                combat, {}, abilities, economy);
            expect(static_cast<bool>(created), "concentrate sparse lifecycle fixture creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto session = std::move(created).value();
            expect(static_cast<bool>(session.submit(command(0, 1, 0, 1, tactical::BuyPayload{option.type}))), "lifecycle build submits");
            if (scenario != 2) expect(static_cast<bool>(session.submit(command(0, 1, 2, 1,
                tactical::AbilityPayload{ordinary.kind, tactical::AbilityAction::activate, 2}))), "lifecycle focus submits");
            if (scenario != 0) {
                tactical::PlayerCommand arrival{{3, 1, scenario == 2 ? 1U : 3U}, {}, tactical::ReinforcePayload{corvette_type,
                    {Fixed::from_raw(tactical::arrival_tail(0).raw() + units(100).raw()), units(200), {}}}};
                expect(static_cast<bool>(session.submit(arrival)), "same-frame arrival submits");
            }
            if (scenario == 2) expect(static_cast<bool>(session.submit(command(3, 1, 2, 1,
                tactical::AbilityPayload{ordinary.kind, tactical::AbilityAction::activate, 2}))), "lifecycle focus submits");
            if (scenario == 1) expect(static_cast<bool>(session.submit(command(3, 2, 0, 2,
                tactical::DamagePayload{units(100000)}))), "target death submits alongside birth");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 6; ++frame) {
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), "sparse lifecycle step succeeds");
                if (!step) { std::cerr << step.error().message << '\n'; break; }
                hashes.push_back(step.value().state_sha256);
                if (scenario == 0 && frame >= 3) {
                    expect(step.value().asteroid_impacts.size() == 1 && session.durability_state(2)->hull == units(3600 - 30 * (frame + 1)),
                        "station bonus rebuild preserves active concentrate defense for cached asteroid damage");
                }
                if (frame == 3 && scenario != 0) {
                    const auto state = session.ability_state(1);
                    expect(session.arrivals().size() == 1, "fixture actually created a movement-locked reinforcement");
                    if (scenario == 1) expect(session.units().size() == initial.units.size() && state && !state->slots[0].active
                        && state->concentrate_recruits.empty() && state->special.slots[0].targets.empty(),
                        "target death plus birth at unchanged census releases concentrate in the same tick");
                    else expect(state && state->slots[0].active && state->concentrate_recruits == std::vector<eawr::sim::EntityId>{3},
                        "source inside radius and newly arriving movement-locked ship are excluded from the batch");
                }
            }
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "sparse lifecycle hashes equal at 1/2/4/8 workers");
        }
    }
}

void test_nested_special_handlers() {
    using Kind = tactical::SpecialAbilityKind;
    using Style = tactical::SpecialActivationStyle;
    tactical::SpecialAbilityProfile profile;
    profile.name = "typed-handler";
    profile.kind = Kind::tractor_beam;
    profile.style = Style::user_input;
    profile.initially_enabled = true;
    profile.service_interval = tactical::special_service_interval(profile.kind);
    auto profiles = std::vector(3, profile);
    profiles[1].causes_despawn = true;
    profiles[2].service_interval = 0;
    tactical::SpecialActivationContext context;
    context.owner = 7; context.target = 8;
    auto state = tactical::initial_special_abilities(profiles);
    SpecialProbe probe;
    expect(tactical::activate_special_abilities(profiles, state, Style::user_input, context, true, probe) == 1
        && probe.applied == std::vector<std::size_t>{0, 1} && state.slots[1].despawn_success,
        "WHE-10: first success follows declared order; successful despawn is recorded");
    expect(std::all_of(state.slots.begin(), state.slots.end(), [](const auto& slot) { return !slot.context; }),
        "WHE-10: activation context is cleared after failed and successful Apply");
    state = tactical::initial_special_abilities(profiles); probe.applied.clear();
    expect(tactical::activate_special_abilities(profiles, state, Style::user_input, context, false, probe) == 2
        && probe.applied == std::vector<std::size_t>{0, 1, 2}, "WHE-10: activate-all keeps visiting after success");
    for (unsigned gate = 0; gate < 7; ++gate) {
        state = tactical::initial_special_abilities(profiles); probe = SpecialProbe{};
        auto requested = Style::user_input;
        if (gate == 0) requested = Style::space_automatic;
        if (gate == 1) probe.is_ready = false;
        if (gate == 2) probe.mode_ok = false;
        if (gate == 3) for (auto& slot : state.slots) slot.enabled = false;
        if (gate == 4) probe.target_ok = false;
        if (gate == 5) probe.is_available = false;
        if (gate == 6) probe.succeeds = false;
        expect(tactical::activate_special_abilities(profiles, state, requested, context, false, probe) == 0
            && std::none_of(state.slots.begin(), state.slots.end(), [](const auto& slot) { return slot.despawn_success || slot.context; }),
            "WHE-10: each activation gate blocks success and despawn");
        if (gate != 6) expect(probe.applied.empty(), "WHE-10: failed gates do not reach Apply");
    }
    tactical::SpecialAbilityFilter filter;
    filter.applicable_types = {42}; filter.excluded_types = {42, 43};
    filter.applicable_categories = 1; filter.excluded_categories = 2;
    expect(tactical::special_type_matches(filter, 42, 2), "WHE-14: explicit type bypasses categories and both exclusions");
    expect(!tactical::special_type_matches(filter, 43, 1) && !tactical::special_type_matches(filter, 44, 3)
        && !tactical::special_type_matches(filter, 45, 0) && tactical::special_type_matches(filter, 45, 1),
        "WHE-14: category overlap is required, exclusions apply and named variants are not expanded");
    state = tactical::initial_special_abilities(profiles); probe = SpecialProbe{};
    state.slots[0].next_service_frame = 100; state.slots[1].enabled = false;
    expect(tactical::service_special_abilities(profiles, state, context, 99, probe) == 0, "WHE-09: not due means no call");
    expect(tactical::service_special_abilities(profiles, state, context, 900, probe) == 1
        && state.slots[0].next_service_frame == 901 && probe.serviced == std::vector<std::size_t>{0},
        "WHE-09: missed service invokes once and schedules current plus interval; disabled/zero interval skip");
    expect(tactical::service_special_abilities(profiles, state, context, 900, probe) == 0,
        "WHE-09: no second invocation at the same frame");
    for (unsigned exit = 0; exit < 5; ++exit) {
        state = tactical::initial_special_abilities(profiles); probe = SpecialProbe{};
        auto blocked = context;
        if (exit == 0) blocked.owner_exists = false;
        if (exit == 1) blocked.type_exists = false;
        if (exit == 2) blocked.death_clone = true;
        if (exit == 3) blocked.map_editor = true;
        if (exit == 4) for (auto& slot : state.slots) slot.cancelled = true;
        expect(tactical::service_special_abilities(profiles, state, blocked, 900, probe) == 0 && probe.serviced.empty(),
            "WHE-09: absent owner/type, clone, editor and all-cancelled exits");
    }
    state = tactical::initial_special_abilities(profiles); probe = SpecialProbe{};
    state.slots[0].targets = {8, 9, 99}; state.slots[1].despawn_success = true; state.slots[1].targets = {10};
    state.slots[2].enabled = false;
    tactical::delete_special_target(state, 9);
    tactical::delete_special_owner(profiles, state, context.mode, probe);
    expect(state.service_cancelled && state.slots[0].targets.empty() && state.slots[1].targets == std::vector<eawr::sim::EntityId>{10}
        && probe.removed == std::vector<eawr::sim::EntityId>{8} && probe.terminated == std::vector<std::size_t>{0},
        "WHE-12: target deletion forgets IDs; owner deletion removes live effects and skips disabled/successful-despawn handlers");
    expect(tactical::service_special_abilities(profiles, state, context, 901, probe) == 0, "WHE-12: deleted owner cancels service");
    state = tactical::initial_special_abilities(profiles); probe = SpecialProbe{}; probe.mode_ok = false;
    tactical::delete_special_owner(profiles, state, context.mode, probe);
    expect(probe.terminated.empty(), "WHE-12: owner termination requires an appropriate mode");

    // Immutable copied inputs and disjoint owner staging; commit encoded records in owner order.
    const std::vector inputs(64, tactical::initial_special_abilities(profiles));
    std::vector<std::uint8_t> baseline;
    std::vector<std::uint8_t> session_baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers, eawr::platform::ThreadWorkerAdapter::Dispatch::always_pool);
        auto staged = inputs;
        std::vector<std::size_t> calls(inputs.size());
        expect(static_cast<bool>(executor.execute_phase("nested-special-handlers", 16, [&](const std::size_t partition) {
            for (auto owner = partition; owner < staged.size(); owner += 16) {
                SpecialProbe local;
                auto local_context = context; local_context.owner = owner + 1;
                calls[owner] = tactical::service_special_abilities(profiles, staged[owner], local_context, 900, local);
                calls[owner] += tactical::activate_special_abilities(profiles, staged[owner], Style::user_input, local_context, false, local);
                if (!local.context_valid || !local.schedule_valid) calls[owner] = 0;
            }
        })), "nested handler partitioned phase succeeds");
        std::vector<std::uint8_t> bytes;
        for (const auto& owner : staged) tactical::append_special_abilities(bytes, owner);
        expect(std::all_of(calls.begin(), calls.end(), [](const auto count) { return count == 4; }),
            "nested handler work is bounded by two due and three declared activation slots per owner");
        if (workers == 1) baseline = bytes;
        else expect(bytes == baseline, "nested handler state is identical at 1/2/4/8 workers");
        auto table = abilities({1}); table.profiles.front().special = profiles;
        table.autofire_defaults = {1};
        table.profiles.front().abilities.front().supports_autofire = true;
        auto created = tactical::TacticalSession::create(setup(), {}, durability(), motion(), std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "nested metadata session validates");
        if (!created) continue;
        auto world = std::move(created).value();
        const auto combined = world.ability_state(1);
        expect(combined && combined->slots.front().autofire && combined->special.slots.size() == 3,
            "AB-45 and WHE-09: creation initializes autofire and nested slots together");
        expect(static_cast<bool>(world.step(executor)), "nested metadata participates in staged session copies");
        const auto session_bytes = world.canonical_state_bytes();
        constexpr std::string_view tag = "SPAB";
        expect(std::search(session_bytes.begin(), session_bytes.end(), tag.begin(), tag.end()) != session_bytes.end()
            && world.ability_state(1)->special.slots.size() == 3, "SPAB binds nested owner state after ordered commit");
        if (workers == 1) session_baseline = session_bytes;
        else expect(session_bytes == session_baseline, "SPAB session state is identical at 1/2/4/8 workers");
    }
}

void test_barrage_proxy_and_override() {
    tactical::AbilityProfile barrage;
    barrage.kind = tactical::AbilityKind::barrage;
    barrage.expiration_frames = 300;
    barrage.recharge_frames = 1200;
    barrage.modifiers.fire_rate = units(3);
    barrage.fixed_inaccuracy = units(320);
    barrage.target_z_offset = units(-150);
    barrage.barrage_target_type = 77;
    const tactical::AbilityTable ability_data{{unit_of(corvette_type, barrage)}, {1}};
    tactical::CombatTable combat;
    tactical::CombatProfile ship;
    ship.type_id = corvette_type; ship.category_bits = 1;
    tactical::WeaponProfile weapon;
    weapon.range = units(1500); weapon.pulse_count = 5; weapon.pulse_delay_frames = 30;
    tactical::ShotProfile ordinary;
    ordinary.damage = units(5); ordinary.damage_type = 0;
    ordinary.speed = units(7); ordinary.max_travel = units(1500);
    ordinary.energy_per_shot = units(20);
    weapon.shot = ordinary;
    auto override_shot = ordinary;
    override_shot.energy_per_shot = {};
    override_shot.damage = units(9); override_shot.speed = units(12);
    override_shot.blast.damage = units(150); override_shot.blast.radius = units(200);
    override_shot.blast.dropoff = true; override_shot.blast.tiers = 5;
    tactical::FlightProfile flight;
    flight.kind = tactical::FlightKind::rocket; flight.target_radius = true;
    flight.authored_distance = units(3000); flight.curve_distance = units(500); flight.straight_distance = units(500);
    override_shot.flight = flight;
    weapon.barrage_shot = override_shot;
    ship.weapons.push_back(weapon);
    tactical::CombatProfile marker; marker.type_id = 77; marker.category_bits = 1;
    combat.profiles = {ship, marker};
    auto start = setup(); start.units[0].position = at(0, 0);
    const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(2000)}};
    const auto activate = command(0, 1, 1, 1, tactical::AreaAbilityPayload{tactical::AbilityKind::barrage, at(500, 0)});
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt,
            combat, {}, ability_data);
        expect(static_cast<bool>(created), "WAD-38: barrage session binds");
        if (!created) return;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(activate)), "WAD-38: point command submits");
        expect(static_cast<bool>(session.submit(command(2, 1, 2, 1,
            tactical::AreaAbilityPayload{tactical::AbilityKind::barrage, at(700, 0)}))),
            "WAD-38: repeated activation submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::size_t override_fired = 0;
        for (std::uint64_t tick = 0; tick < 305; ++tick) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "WAD-38: barrage step succeeds");
            if (!stepped) return;
            hashes.push_back(stepped.value().state_sha256);
            if (tick == 0 || tick == 2) {
                const auto units_now = session.units();
                const auto proxy = std::find_if(units_now.begin(), units_now.end(),
                    [](const tactical::UnitState& unit) { return unit.barrage_source == 1; });
                expect(proxy != units_now.end() && proxy->owner == 2 && proxy->position == at(500, 0, -150),
                    "WAD-38: enemy proxy retains Z offset; repeated activation does not replace it");
                expect(units_now.size() == start.units.size() + 1, "WAD-38: one live proxy");
                expect(proxy == units_now.end() || !session.durability_state(proxy->entity_id),
                    "WAD-38: immune marker has no damageable hull");
            }
            for (const auto& event : stepped.value().snapshot->combat_events()) {
                if (event.kind != tactical::CombatEventKind::weapon_fired || event.shooter != 1) continue;
                ++override_fired;
                expect(event.outcome == tactical::fired_barrage_shot, "WAD-38: override event uses ordinary fire");
                expect(std::abs(event.aim.x.raw() - units(500).raw()) <= units(320).raw()
                    && std::abs(event.aim.y.raw()) <= units(320).raw()
                    && std::abs(event.aim.z.raw() - units(-150).raw()) <= units(320).raw(),
                    "WAD-38: fixed scatter bounds are independent of travel range");
            }
            for (const auto& shot : stepped.value().snapshot->projectiles()) {
                expect(shot.damage == units(9) && shot.speed == units(12)
                    && shot.blast.damage == units(150) && shot.blast.radius == units(200)
                    && shot.blast.dropoff && shot.blast.tiers == 5 && shot.flight
                    && shot.flight->profile.authored_distance == units(3000),
                    "WAD-38: override keeps inherited area budget and separate authored flight distance");
            }
        }
        expect(override_fired > 0, "WAD-38: override uses its zero energy cost instead of the ordinary shot's cost");
        const auto state = session.ability_state(1);
        expect(state && !state->slots[0].active && state->slots[0].target == 0
            && state->slots[0].ready_tick == 1500, "WAD-38: expiry clears proxy/override and starts full recharge");
        expect(session.units().size() == start.units.size(), "WAD-38: expired proxy is removed");
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "WAD-38: proxy/override hashes match worker counts");
        auto encoded = tactical::write_replay(session.record());
        auto parsed = encoded ? tactical::parse_replay(encoded.value())
            : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(parsed && parsed.value() == session.record(), "WAD-38: point commands round-trip without changing entity-target codec");
        if (parsed) {
            auto playback = tactical::TacticalSession::from_replay(parsed.value(), sensors, durability(), motion(),
                std::nullopt, combat, {}, ability_data);
            expect(static_cast<bool>(playback), "WAD-38: retained point replay binds");
            if (playback) {
                std::vector<std::string> replay_hashes;
                while (playback.value().completed_tick() < parsed.value().final_tick_count) {
                    const auto frame = playback.value().step(executor);
                    expect(static_cast<bool>(frame), "WAD-38: point replay step succeeds");
                    if (!frame) break;
                    replay_hashes.push_back(frame.value().state_sha256);
                }
        expect(replay_hashes == hashes, "WAD-38: recorded point activation/expiry reproduces every hash");
            }
        }
    }
    {
        auto costly = combat;
        costly.profiles[0].weapons[0].shot->energy_per_shot = {};
        costly.profiles[0].weapons[0].barrage_shot->energy_per_shot = units(20);
        auto blocked = tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt,
            costly, {}, ability_data);
        expect(static_cast<bool>(blocked), "WAD-38: costly override session binds");
        if (blocked) {
            expect(static_cast<bool>(blocked.value().submit(activate)), "WAD-38: costly override activates");
            const eawr::platform::ThreadWorkerAdapter executor(1);
            for (std::uint64_t frame = 0; frame < 10; ++frame) {
                const auto stepped = blocked.value().step(executor);
                expect(static_cast<bool>(stepped), "WAD-38: costly override step succeeds");
                if (!stepped) break;
                expect(std::none_of(stepped.value().snapshot->combat_events().begin(),
                    stepped.value().snapshot->combat_events().end(), [](const tactical::CombatEvent& event) {
                        return event.shooter == 1 && event.kind == tactical::CombatEventKind::weapon_fired;
                    }), "WAD-38: a free ordinary shot cannot bypass the selected override's energy cost");
            }
        }
    }
    auto created = tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt,
        combat, {}, ability_data);
    if (!created) return;
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.submit(command(0, 1, 1, 1,
        tactical::AreaAbilityPayload{tactical::AbilityKind::barrage, at(9000, 0)}))), "WAD-38: fogged command submits");
    const eawr::platform::ThreadWorkerAdapter executor(1);
    auto rejected = session.step(executor);
    expect(rejected && rejected.value().snapshot->events()[0].reason == tactical::RejectReason::invalid_position
        && session.units().size() == start.units.size(), "WAD-38: fogged point creates no proxy");
    expect(static_cast<bool>(session.submit(command(1, 1, 2, 1,
        tactical::AreaAbilityPayload{tactical::AbilityKind::barrage, at(500, 0)}))), "WAD-38: visible point submits");
    expect(static_cast<bool>(session.submit(command(2, 1, 3, 1,
        tactical::AbilityPayload{tactical::AbilityKind::barrage, tactical::AbilityAction::deactivate}))),
        "WAD-38: cancellation submits");
    expect(static_cast<bool>(session.step(executor)) && static_cast<bool>(session.step(executor)),
        "WAD-38: activation/cancellation step");
    const auto canceled = session.ability_state(1);
    expect(canceled && !canceled->slots[0].active && canceled->slots[0].target == 0
        && session.units().size() == start.units.size(), "WAD-38: cancellation removes proxy without a dangling override");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto mortal = tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt,
            combat, {}, ability_data);
        expect(static_cast<bool>(mortal), "WAD-38: source-death session binds");
        if (!mortal) continue;
        expect(static_cast<bool>(mortal.value().submit(activate)), "WAD-38: source activates before death");
        expect(static_cast<bool>(mortal.value().submit(command(1, 1, 2, 1,
            tactical::DamagePayload{units(10000)}))), "WAD-38: lethal source damage submits");
        const eawr::platform::ThreadWorkerAdapter death_executor(workers);
        expect(static_cast<bool>(mortal.value().step(death_executor))
            && static_cast<bool>(mortal.value().step(death_executor)), "WAD-38: lethal source damage steps");
        const auto survivors = mortal.value().units();
        expect(std::none_of(survivors.begin(), survivors.end(), [](const tactical::UnitState& unit) {
            return unit.entity_id == 1 || unit.barrage_source == 1;
        }), "WAD-38: source death removes its target proxy in the same ordered commit");
    }
}

void test_spawned_hero_abilities() {
    using K = tactical::AbilityKind;
    for (const auto kind : {K::harmonic_bomb, K::weaken_enemy}) for (const bool zero : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(100, 0), math::identity_quat(), {}},
                {3, corvette_type, 1, at(100, 0), math::identity_quat(), {}}};
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
            tactical::AbilityProfile ordinary;
            ordinary.kind = kind; ordinary.recharge_frames = 20;
            ordinary.spawned = tactical::SpawnedAbilityProfile{};
            auto& spawned = *ordinary.spawned;
            spawned.type = 0x1122334455667788ULL; spawned.damage_type = 0;
            spawned.countdown_frames = kind == K::harmonic_bomb ? 3 : 0;
            spawned.reach = units(300);
            if (kind == K::harmonic_bomb) { spawned.blast.damage = units(50); spawned.blast.radius = units(120); spawned.blast.max_delay = {}; }
            else {
                spawned.weaken.on_detonation = true; spawned.weaken.radius = units(120);
                spawned.weaken.duration_frames = 3; spawned.weaken.categories = 2;
                spawned.weaken.take_damage_increase = decimal("0.5");
                spawned.weaken.cause_damage_reduction = decimal("0.2");
            }
            tactical::AbilityTable table; table.profiles = {unit_of(plain_type, ordinary)};
            tactical::CombatTable combat;
            for (const auto type : {corvette_type, frigate_type, plain_type}) {
                tactical::CombatProfile profile; profile.type_id = type; profile.category_bits = 2;
                profile.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
                if (type == frigate_type) {
                    profile.max_attack_distance = units(300);
                    tactical::WeaponProfile weapon; weapon.range = units(300); weapon.pulse_count = 1;
                    weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
                    weapon.cone_width = units(360); weapon.cone_height = units(160);
                    weapon.shot = tactical::ShotProfile{units(1), 0, units(1), units(300), true, true, {}};
                    profile.weapons = {weapon};
                }
                combat.profiles.push_back(profile);
            }
            auto created = tactical::TacticalSession::create(initial,
                std::vector<tactical::SensorProfile>{{corvette_type, units(1000)}, {frigate_type, units(1000)}, {plain_type, units(1000)}},
                health, {}, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "spawned hero fixture creates");
            if (!created) continue;
            auto world = std::move(created).value();
            tactical::AbilityPayload input{kind, tactical::AbilityAction::activate};
            if (kind == K::weaken_enemy) input.position = zero ? math::Vec3{} : at(100, 0);
            expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, input))), "spawn input accepted");
            expect(static_cast<bool>(world.submit(command(1, 1, 1, 1, input))), "cooldown input queues for rejection");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            bool reduced_shot = false;
            std::uint64_t spawned_projectile = 0;
            for (unsigned frame = 0; frame < 7; ++frame) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "spawned hero step succeeds");
                if (!step) { std::cerr << step.error().message << '\n'; break; }
                hashes.push_back(step.value().state_sha256);
                const auto spawns = step.value().snapshot->ability_spawns();
                if (frame == 0 && !spawns.empty()) spawned_projectile = spawns.front().id;
                expect(std::none_of(step.value().snapshot->combat_events().begin(),
                    step.value().snapshot->combat_events().end(), [&](const auto& event) {
                        return event.kind == tactical::CombatEventKind::projectile_expired
                            && event.target == spawned_projectile;
                    }), "WHE-62/WAD-07: a stationary ability spawn has only its hero detonation presentation");
                if (frame == 0) expect(spawns.size() == 1 && spawns.front().type == spawned.type
                    && spawns.front().position == (kind == K::harmonic_bomb || zero ? at(0, 0) : at(100, 0))
                    && !world.ability_state(1)->slots[0].active && world.ability_state(1)->slots[0].ready_tick == 20,
                    "instant spawn retains type, position and cooldown without an active slot");
                if (frame == 1) expect(std::any_of(step.value().snapshot->events().begin(), step.value().snapshot->events().end(),
                    [](const auto& event) { return event.kind == tactical::EventKind::order_rejected; }), "recharge rejects another spawn");
                if (kind == K::weaken_enemy && frame == 1) expect(spawns.size() == 1 && spawns.front().detonated
                    && spawns.front().recipients == std::vector<tactical::WeakenRecipient>{{2, 4}},
                    "weaken admits the different-owner collidable combat recipient and keeps its expiry");
                for (const auto& projectile : step.value().snapshot->projectiles()) if (projectile.shooter == 2
                    && projectile.source_damage_factor == math::Fixed::from_raw(one + decimal("-0.2").raw())) reduced_shot = true;
                if (frame >= 4) expect(spawns.empty(), "detonated metadata releases after countdown or final recipient expiry");
                if (kind == K::harmonic_bomb && frame == 2) expect(world.durability_state(2)->hull == units(3600), "bomb waits its authored countdown");
                if (kind == K::harmonic_bomb && frame == 3) expect(world.durability_state(2)->hull < units(3600), "bomb countdown delivers ordinary area damage");
            }
            if (kind == K::weaken_enemy) expect(reduced_shot, "timed weaken contributes to captured outgoing projectile damage");
            const auto record = world.record();
            const auto encoded = tactical::write_replay(record);
            const auto parsed = encoded ? tactical::parse_replay(encoded.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == record, "world-point ability extension round-trips");
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "spawn and timed-status hashes equal on 1/2/4/8 workers");
        }
    }
}

void test_hero_beams() {
    using K = tactical::AbilityKind;
    for (const auto kind : {K::energy_weapon, K::tractor_beam}) {
        for (const auto scenario : {0U, 1U, 2U, 3U, 4U, 5U}) {
            std::vector<std::string> baseline;
            for (const auto workers : {1U, 2U, 4U, 8U}) {
                auto initial = setup();
                initial.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                    {2, frigate_type, 2, at(scenario == 1 ? 300 : scenario == 2 ? 5 : 100, 0, 100), math::identity_quat(), {}}};
                auto health = durability();
                for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
                if (scenario == 3 && kind == K::energy_weapon)
                    health.profiles[1].hardpoints = {{tactical::HardpointRole::other, true, units(50)},
                        {tactical::HardpointRole::other, true, units(100)}};
                tactical::AbilityProfile ordinary;
                ordinary.kind = kind; ordinary.recharge_frames = 20;
                ordinary.expiration_frames = kind == K::energy_weapon ? 4 : 0;
                ordinary.gui_activated_ability_name = "beam";
                ordinary.modifiers.speed = decimal("0.8");
                ordinary.supports_autofire = true;
                auto holder = unit_of(corvette_type, ordinary);
                tactical::SpecialAbilityProfile nested;
                nested.name = "beam"; nested.style = tactical::SpecialActivationStyle::user_input;
                nested.kind = kind == K::energy_weapon ? tactical::SpecialAbilityKind::energy_weapon
                    : tactical::SpecialAbilityKind::tractor_beam;
                nested.service_interval = 1; nested.beam_min_range = units(5); nested.beam_max_range = units(300);
                nested.damage_per_frame = units(50);
                nested.target_speed_decrease = decimal("0.2"); nested.doubled_speed_categories = 2;
                nested.filter.applicable_categories = 2;
                holder.special = {nested};
                tactical::AbilityTable table; table.profiles = {holder}; table.beam_damage_type = 0;
                tactical::CombatProfile source; source.type_id = corvette_type; source.category_bits = 1;
                tactical::CombatProfile target; target.type_id = frigate_type; target.category_bits = 2;
                target.hero = scenario == 4;
                if (scenario == 3 && kind == K::energy_weapon)
                    target.hardpoints = {{0, at(-10, 0), true}, {1, at(10, 0), true}};
                tactical::CombatTable combat; combat.profiles = {source, target};
                auto movement = motion();
                if (kind == K::tractor_beam) {
                    movement.profiles[1].max_speed = units(2);
                    movement.profiles[1].acceleration = units(10);
                    movement.profiles[1].deceleration = units(10);
                }
                auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, combat, {}, table);
                expect(static_cast<bool>(created), "WHE-26/27: beam session binds typed ordinary and nested profiles");
                if (!created) continue;
                auto world = std::move(created).value();
                if (kind == K::tractor_beam && scenario != 1 && scenario != 2) expect(static_cast<bool>(world.submit(command(0, 2, 0, 2,
                    tactical::MovePayload{at(scenario == 1 ? 300 : scenario == 2 ? 5 : 100, 100, 100)}))),
                    "tractor fixture target starts a move");
                expect(static_cast<bool>(world.submit(command(0, 1, 0, 1,
                    tactical::AbilityPayload{kind, tactical::AbilityAction::activate, 2, scenario == 3 ? 99U : tactical::no_hardpoint}))),
                    "beam targeted command validates, including an invalid requested hardpoint for fallback");
                if (scenario == 5) expect(static_cast<bool>(world.submit(command(2, 1, 1, 1,
                    tactical::AbilityPayload{kind, tactical::AbilityAction::deactivate}))), "beam release command submits");
                const eawr::platform::ThreadWorkerAdapter executor(workers);
                std::vector<std::string> hashes;
                for (unsigned frame = 0; frame < 6; ++frame) {
                    const auto stepped = world.step(executor);
                    expect(static_cast<bool>(stepped), "beam service tick succeeds");
                    if (!stepped) break;
                    hashes.push_back(stepped.value().state_sha256);
                    if (frame == 0) {
                        const auto slot = world.ability_state(1)->slots.front();
                        expect(slot.active == (scenario != 2), "WHE-58: adjusted maximum equality accepted, positive minimum equality refused; Z ignored");
                        if (scenario == 3 && kind == K::energy_weapon)
                            expect(slot.target_hardpoint == 0, "WHE-59: invalid explicit hardpoint falls back to nearest live targetable point");
                    }
                    if (kind == K::tractor_beam && frame <= 2) {
                        const auto instances = stepped.value().snapshot->instances();
                        const auto target_instance = std::find_if(instances.begin(), instances.end(), [](const auto& value) { return value.entity_id == 2; });
                        expect(target_instance != instances.end() && target_instance->in_tractor_beam == (scenario != 2 && !(frame == 2 && scenario == 5)),
                            "WHE-27: tractor held flag follows active source entries and release");
                        if ((frame == 0 || (frame == 2 && scenario == 5)) && scenario != 1 && scenario != 2) {
                            const auto moving = world.motion_state(2);
                            Fixed peak{};
                            if (moving) for (const auto& node : moving->nodes) peak = std::max(peak, node.speed);
                            const auto decrease = scenario == 2 || (frame == 2 && scenario == 5) ? Fixed{}
                                : math::Fixed::from_raw(nested.target_speed_decrease.raw() * (scenario == 4 ? 1 : 2));
                            const auto expected = math::multiply(units(2), Fixed::from_raw(one - decrease.raw())).value();
                            expect(peak == expected, "WHE-60: tractor replans moving target with doubled non-hero speed loss, hero exemption and release restoration");
                        }
                    }
                    if (frame == 2 && scenario == 3 && kind == K::energy_weapon)
                        expect(!world.ability_state(1)->slots.front().active && world.durability_state(2)->hardpoints[1] == units(100),
                            "WHE-26/59: dead selected hardpoint releases without retargeting the survivor");
                }
                const auto final = world.durability_state(2);
                if (kind == K::energy_weapon && scenario != 3) {
                    // The tick's impacts precede its commands; tick-2 release follows that tick's service.
                    const auto hits = scenario == 2 ? 0 : scenario == 5 ? 2 : 3;
                    expect(final && final->hull == units(3600 - hits * 50),
                        "WHE-26: Damage_Per_Frame enters direct ordinary damage once per service, stopping at release or expiry");
                }
                if (scenario == 5) expect(!world.ability_state(1)->slots.front().active
                    && world.ability_state(1)->slots.front().target == 0, "beam release clears tracked target and nested recipients");
                if (baseline.empty()) baseline = hashes;
                else expect(hashes == baseline, "beam state hashes equal at 1/2/4/8 workers");
                const auto record = world.record();
                const auto encoded = tactical::write_replay(record);
                const auto parsed = encoded ? tactical::parse_replay(encoded.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
                expect(parsed && parsed.value() == record, "beam commands round-trip through the existing ability opcode");
            }
        }
    }
}

void test_hero_wingmen() {
    using K = tactical::AbilityKind;
    constexpr tactical::TypeId escort = 23;
    for (const unsigned parent_kind : {0U, 1U, 2U}) for (const bool ordinary : {false, true}) for (const bool guard : {false, true}) {
        const bool parent = parent_kind == 1;
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, plain_type, 2, at(0, 0), math::identity_quat(), {}},
                {11, xwing_type, 1, at(0, 0), math::identity_quat(), {}}};
            if (parent) {
                initial.units.insert(initial.units.end(), {{10, xwing_squadron, 1, at(0, 0), math::identity_quat(), {}},
                    {12, escort, 1, at(-100, 0), math::identity_quat(), {}},
                    {13, escort, 1, at(100, 0), math::identity_quat(), {}}});
                std::sort(initial.units.begin(), initial.units.end(), [](const auto& a, const auto& b) { return a.entity_id < b.entity_id; });
                initial.squadrons = {{10, {11, 12, 13}}};
            }
            auto movement = hangar_motion();
            movement.squadrons.spawners.clear();
            movement.squadrons.craft.front().max_speed = decimal("0.0001");
            movement.squadrons.craft.front().min_speed = decimal("0.0001");
            movement.squadrons.craft.front().thrust = decimal("0.0001");
            auto wing = movement.squadrons.craft.front(); wing.type_id = escort;
            movement.squadrons.craft.push_back(wing);
            auto& team = movement.squadrons.squadrons.front();
            const auto authored_count = parent_kind == 2 ? 17U : 3U;
            team.members.assign(authored_count, escort); team.members.front() = xwing_type;
            team.offsets.assign(authored_count, at(100, 0)); team.offsets.front() = at(0, 0);
            if (parent_kind == 2) {
                movement.squadrons.squadrons.insert(movement.squadrons.squadrons.begin(),
                    {xwing_type, {xwing_type}, {at(0, 0)}, {}, {}, {}, {}});
            }
            auto health = durability();
            health.profiles = {{plain_type, units(1000), {}, false, {}},
                {xwing_type, units(1000), {}, false, {}}, {escort, units(1000), {}, false, {}}};
            tactical::CombatTable combat;
            for (const auto type : {plain_type, xwing_type, escort}) {
                tactical::CombatProfile item; item.type_id = type;
                item.category_bits = 2;
                item.redirect_damage_to_teammates = type == xwing_type || (guard && type == escort);
                item.collision = tactical::CollisionBox{at(-3, -3, -3), at(3, 3, 3)};
                combat.profiles.push_back(item);
            }
            tactical::AbilityProfile replenish;
            replenish.kind = K::replenish_wingmen; replenish.recharge_frames = 6; replenish.replenish_team = xwing_squadron;
            tactical::AbilityProfile mode; mode.kind = K::invulnerability; mode.expiration_frames = 100;
            mode.modifiers.take_damage = decimal("0.1");
            auto leader = unit_of(xwing_type, mode); leader.abilities.push_back(replenish);
            tactical::AbilityProfile beam; beam.kind = K::energy_weapon; beam.recharge_frames = 5;
            beam.expiration_frames = 2; beam.effective_radius = units(10000); beam.gui_activated_ability_name = "routing-beam";
            auto source = unit_of(plain_type, beam);
            tactical::SpecialAbilityProfile service; service.name = "routing-beam";
            service.kind = tactical::SpecialAbilityKind::energy_weapon; service.service_interval = 1;
            service.style = tactical::SpecialActivationStyle::user_input; service.damage_per_frame = units(60);
            service.beam_max_range = units(10000); service.filter.applicable_categories = 2;
            source.special = {service};
            tactical::AbilityTable table; auto escort_mode = mode; escort_mode.modifiers.take_damage = decimal("0.5");
            table.profiles = {source, leader, unit_of(escort, escort_mode)}; table.beam_damage_type = 0;
            auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "WHE-63/64 wingmen fixture creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            const auto fill = tactical::AbilityPayload{K::replenish_wingmen, tactical::AbilityAction::activate};
            expect(static_cast<bool>(world.submit(command(0, 1, 0, 11, fill))), "wingmen activate submits");
            expect(static_cast<bool>(world.submit(command(0, 1, 1, 11,
                tactical::AbilityPayload{K::invulnerability, tactical::AbilityAction::activate}))), "leader take mode submits");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            auto first = world.step(executor);
            expect(static_cast<bool>(first), "wingmen first frame completes");
            if (!first) { std::cerr << first.error().message << '\n'; continue; }
            hashes.push_back(first.value().state_sha256);
            expect(world.squadrons().size() == 1 && world.squadrons()[0].members.size() == authored_count,
                "WHE-63 absent parent creates indices one onward, full parent retains members");
            auto state = world.ability_state(11);
            expect(state && state->slots[1].ready_tick == (parent ? 0U : 6U),
                "WHE-63 full team returns before recharge, newly formed team recharges");
            const auto dead = world.squadrons()[0].members[1];
            expect(static_cast<bool>(world.submit(command(1, 1, 0, dead, tactical::DamagePayload{units(2000), tactical::hull_target}))),
                "one escort destruction submits");
            auto killed = world.step(executor); expect(static_cast<bool>(killed), "escort destruction completes");
            if (!killed) continue;
            hashes.push_back(killed.value().state_sha256);
            expect(world.squadrons()[0].members.size() == authored_count - 1, "destroyed escort is pruned");
            const auto refill_tick = parent ? 2U : 6U;
            expect(static_cast<bool>(world.submit(command(refill_tick, 1, 0, 11, fill))), "partial team refill submits");
            for (unsigned frame = 2; frame <= refill_tick; ++frame) {
                auto step = world.step(executor); expect(static_cast<bool>(step), "refill frame completes");
                if (step) hashes.push_back(step.value().state_sha256);
            }
            expect(world.squadrons().size() == 1 && world.squadrons()[0].members.size() == authored_count
                && world.squadrons()[0].members.front() == 11 && world.squadrons()[0].members.back() > dead,
                "WHE-63 survivor roster retained and authored escort appended with a fresh stable ID");
            state = world.ability_state(11);
            expect(state && !state->slots[1].active && state->slots[1].ready_tick == refill_tick + 6,
                "WHE-63 replenishment is instantaneous with authored recharge");
            const auto damage_tick = refill_tick + 1;
            unsigned sequence = 0;
            for (const auto id : world.squadrons()[0].members) if (id != 11)
                expect(static_cast<bool>(world.submit(command(damage_tick, 1, sequence++, id,
                    tactical::AbilityPayload{K::invulnerability, tactical::AbilityAction::activate}))), "recipient take mode submits before script damage");
            expect(static_cast<bool>(world.submit(command(damage_tick, ordinary ? 2 : 1, sequence, ordinary ? 1 : 11,
                ordinary ? tactical::CommandPayload{tactical::AbilityPayload{K::energy_weapon, tactical::AbilityAction::activate, 11}}
                    : tactical::CommandPayload{tactical::DamagePayload{units(60), tactical::hull_target}}))), "routed damage submits");
            for (unsigned frame = damage_tick; frame <= damage_tick + 2; ++frame) {
                auto step = world.step(executor); expect(static_cast<bool>(step), "routed damage frame completes");
                if (step) hashes.push_back(step.value().state_sha256);
            }
            const auto leader_health = world.durability_state(11);
            expect(leader_health && leader_health->hull == Fixed::from_raw(units(1000).raw()
                - (ordinary ? 0 : math::multiply(units(60), decimal("0.1")).value().raw())),
                "WHE-64 ordinary hit leaves leader intact; LuaDebugDamage additionally applies leader's take mode");
            for (const auto id : world.squadrons()[0].members) if (id != 11) {
                const auto recipient = world.durability_state(id);
                expect(recipient && recipient->hull == Fixed::from_raw(units(1000).raw() - (guard ? 0
                    : math::multiply(Fixed::from_raw(units(60).raw() / (authored_count - 1)), decimal("0.5")).value().raw())), "WHE-64 raw shares use recipient take modes; recursion guard skips redirecting members");
            }
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "wingmen refill and redirected damage state/events equal at 1/2/4/8 workers");
        }
    }
}

void test_hero_environment_damage_routing() {
    constexpr tactical::TypeId escort = 23;
    std::string reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = setup();
        initial.units = {{1, 99, 2, at(0, 0), math::identity_quat(), {}},
            {10, xwing_squadron, 1, at(0, 0), math::identity_quat(), {}},
            {11, xwing_type, 1, at(0, 0), math::identity_quat(), {}},
            {12, escort, 1, at(-100, 0), math::identity_quat(), {}},
            {13, escort, 1, at(100, 0), math::identity_quat(), {}}};
        initial.squadrons = {{10, {11, 12, 13}}};
        auto movement = hangar_motion(); movement.squadrons.spawners.clear();
        auto wing = movement.squadrons.craft.front(); wing.type_id = escort;
        movement.squadrons.craft.push_back(wing);
        movement.squadrons.squadrons.front().members = {xwing_type, escort, escort};
        movement.squadrons.squadrons.front().offsets = {at(0, 0), at(-100, 0), at(100, 0)};
        movement.avoidance = tactical::AvoidanceRules{units(24), decimal("0.2"), units(100), decimal("0.8"), units(15),
            decimal("0.66"), decimal("1.2"), decimal("0.25"), decimal("1.7"), decimal("0.5"), decimal("0.5"), 3500, 6, 90, 45, units(50)};
        tactical::Footprint victim; victim.type_id = xwing_type; victim.asteroid_damage = true; victim.radius = units(3);
        victim.layer = tactical::SpaceLayer::frigate;
        tactical::Footprint field; field.type_id = 99; field.layer = tactical::SpaceLayer::static_object;
        field.asteroid_field = true; field.obstacle = true; field.radius = units(20);
        movement.footprints = {victim, field};
        auto health = durability(); health.profiles = {{xwing_type, units(1000), {}, false, {}}, {escort, units(1000), {}, false, {}}};
        health.damage->asteroid_damage = units(60); health.damage->asteroid_rate = units(1);
        tactical::CombatProfile leader; leader.type_id = xwing_type; leader.redirect_damage_to_teammates = true;
        tactical::CombatTable combat; combat.profiles = {leader};
        tactical::AbilityProfile mode; mode.kind = tactical::AbilityKind::invulnerability; mode.expiration_frames = 50;
        mode.modifiers.take_damage = decimal("0.1");
        auto recipient_mode = mode; recipient_mode.modifiers.take_damage = decimal("0.5");
        tactical::AbilityTable abilities; abilities.profiles = {unit_of(xwing_type, mode), unit_of(escort, recipient_mode)};
        auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(created), "WHE-64 environmental routing fixture creates");
        if (!created) continue;
        auto world = std::move(created).value();
        unsigned sequence = 0;
        for (const auto id : {11U, 12U, 13U}) expect(static_cast<bool>(world.submit(command(0, 1, sequence++, id,
            tactical::AbilityPayload{mode.kind, tactical::AbilityAction::activate}))), "environmental recipient mode submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto step = world.step(executor);
        expect(static_cast<bool>(step), "environmental routing ordered commit completes");
        if (!step) continue;
        expect(world.durability_state(11)->hull == units(1000) && world.durability_state(12)->hull == units(985)
            && world.durability_state(13)->hull == units(985), "WHE-64 one environmental raw hit routes before modes to both remote worker recipients");
        if (workers == 1) reference = step.value().state_sha256;
        else expect(step.value().state_sha256 == reference, "environmental recipient commit is equal at 1/2/4/8 workers");
    }
}

void test_hunt_destinations() {
    const std::optional<std::array<Fixed, 4>> area = std::array{units(-2000), units(-2000), units(2000), units(2000)};
    const std::vector<tactical::HuntEnemy> enemies = {{at(600, 700, 20), true, false}, {at(-700, -800, 30), false, true}};
    tactical::CombatRandom sensitive(1036, 0, 1, 0xfffe0006U);
    auto preferred = tactical::hunt_destination(at(0, 0, 50), area, units(100), true, enemies,
        [](math::Vec3) { return false; }, sensitive);
    expect(preferred && preferred.value() == enemies[1].position, "WAB-54: force-sensitive preference without offsets");
    tactical::CombatRandom fallback(1036, 0, 1, 0xfffe0006U);
    const std::optional<std::array<Fixed, 4>> inset = std::array{units(-1000), units(-1000), units(1000), units(1000)};
    const auto clamped = tactical::hunt_destination(at(-4000, -4000), inset, units(500), true, {},
        [](math::Vec3) { return false; }, fallback);
    expect(clamped && clamped.value() == at(-750, -750),
        "WAB-54: adjusted fallback clamps by half the craft's authored reveal range");
    std::size_t point_checks = 0;
    bool saw_fog_branch = false;
    for (std::uint64_t seed = 0; seed < 32 && !saw_fog_branch; ++seed) {
        tactical::CombatRandom random(seed, 0, 1, 0xfffe0006U);
        point_checks = 0;
        const auto chosen = tactical::hunt_destination(at(0, 0, 50), area, units(100), false, enemies,
            [&](math::Vec3) { ++point_checks; return true; }, random);
        expect(chosen && chosen.value().z == units(50), "WAB-54: ordinary hunters preserve their height");
        if (point_checks != 0) {
            expect(point_checks == 99, "WAB-54: exactly 99 diagonal fog samples");
            saw_fog_branch = true;
        } else {
            expect(chosen && chosen.value().x > enemies[0].position.x && chosen.value().x < units(1000)
                && chosen.value().y > enemies[0].position.y && chosen.value().y < units(1100),
                "WAB-54: fogged enemy preferred before positive XY offsets");
        }
    }
    expect(saw_fog_branch, "HUNT contract exercises the fog sampling branch");
    tactical::CombatRandom no_bounds(1036, 0, 1, 0xfffe0006U);
    const auto retained = tactical::hunt_destination(at(10, 20, 30), std::nullopt, units(100), true, enemies,
        [](math::Vec3) { return false; }, no_bounds);
    expect(retained && retained.value() == at(10, 20, 30), "WAB-54: no bounds keeps the base position");
}

void test_hunt_ship_order() {
    tactical::AbilityProfile hunt;
    hunt.kind = tactical::AbilityKind::hunt;
    tactical::AbilityTable table;
    table.profiles = {unit_of(corvette_type, hunt)};
    const auto created_setup = setup();
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(created_setup, {}, {}, motion(), std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "WAB-52: lone hunter session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(command(0, 1, 0, 1, tactical::MovePayload{at(2000, -1500)}))), "ship move submits");
        expect(static_cast<bool>(session.submit(command(1, 1, 1, 1, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}))), "ship Hunt submits");
        expect(static_cast<bool>(session.submit(command(32, 1, 2, 1, tactical::AttackPayload{3}))), "replacement attack submits");
        expect(static_cast<bool>(session.submit(command(33, 1, 3, 1, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::autofire_on}))), "autofire command is recordable");
        expect(static_cast<bool>(session.submit(command(34, 1, 4, 1, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}))), "ship Hunt restarts");
        expect(static_cast<bool>(session.submit(command(35, 1, 5, 1, tactical::StopPayload{}))), "ship Stop submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < 70; ++tick) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "lone hunter advances");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            if (tick == 1 || tick == 31) {
                const auto movement = session.motion_state(1);
                expect(movement && movement->kind == tactical::MotionKind::path && movement->target == at(2000, -1500),
                    "WAB-04/53/56: activation and later service preserve a move already under way");
            }
            if (tick == 32) {
                const auto state = session.ability_state(1);
                expect(state && !state->slots[0].active && state->slots[0].ready_tick == 0,
                    "WAB-51: attack cancels a lone hunter without recharge");
            }
            if (tick == 33) {
                const auto events = stepped.value().snapshot->events();
                expect(std::any_of(events.begin(), events.end(), [](const auto& event) {
                    return event.kind == tactical::EventKind::order_rejected && event.reason == tactical::RejectReason::ability_unavailable;
                }), "WAB-50: Hunt has no autofire");
            }
            if (tick >= 35) {
                const auto state = session.ability_state(1);
                const auto movement = session.motion_state(1);
                expect(state && !state->slots[0].active && state->slots[0].ready_tick == 0,
                    "WAB-51: Stop ends a lone hunter without recharge or later patrol restart");
                expect(movement && movement->kind == tactical::MotionKind::none,
                    "WAB-05/51: Stop ends the lone hunter's movement");
            }
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "lone hunters remain deterministic");
    }
}

void test_hunt_session() {
    tactical::TacticalSetup initial;
    initial.seed = 1036;
    initial.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    initial.units = {{10, xwing_squadron, 1, at(-500, 0), math::identity_quat(), {}},
        {11, xwing_type, 1, at(-500, 0), math::identity_quat(), {}},
        {12, xwing_type, 1, at(-500, 0), math::identity_quat(), {}}};
    initial.squadrons = {{10, {11, 12}}};
    tactical::AbilityProfile hunt;
    hunt.kind = tactical::AbilityKind::hunt;
    tactical::AbilityTable table;
    table.profiles = {unit_of(xwing_type, hunt)};
    const std::vector<tactical::PlayerCommand> orders = {
        command(0, 1, 0, 10, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}),
        command(20, 1, 1, 11, tactical::MovePayload{at(0, 0)}),
        command(30, 1, 2, 10, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}),
        command(40, 1, 3, 10, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::deactivate}),
        command(45, 1, 4, 10, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}),
        command(50, 1, 5, 10, tactical::GuardPayload{at(0, 0)}),
        command(55, 1, 6, 10, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}),
        command(58, 2, 0, 10, tactical::StopPayload{}),
        command(60, 1, 7, 10, tactical::StopPayload{}),
        command(65, 1, 8, 10, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}),
        command(70, 1, 9, 11, tactical::StopPayload{}),
    };
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(initial, {}, {}, hangar_motion(), std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "WAB-50: hunt squadron session is valid");
        if (!created) continue;
        auto session = std::move(created).value();
        for (const auto& order : orders) expect(static_cast<bool>(session.submit(order)), "hunt command is recordable");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < 110; ++tick) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "hunt session step succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto a = session.ability_state(11), b = session.ability_state(12);
            const bool active = tick < 20 || (tick >= 30 && tick < 40) || (tick >= 45 && tick < 50)
                || (tick >= 55 && tick < 60) || (tick >= 65 && tick < 70);
            expect(a && b && a->slots[0].active == active && b->slots[0].active == active,
                "WAB-50/51: accepted orders and switch-off control the whole squadron; foreign Stop does not");
            if (tick == 40 || tick == 60 || tick >= 70) {
                const auto mind = session.squadron_state(10);
                expect(mind && mind->mode == tactical::SquadronMode::idle,
                    "WAB-05/51: switch-off and team/member Stop end patrol across later service ticks");
            }
            if (tick == 58) {
                const auto events = stepped.value().snapshot->events();
                expect(std::any_of(events.begin(), events.end(), [](const auto& event) {
                    return event.kind == tactical::EventKind::order_rejected
                        && event.reason == tactical::RejectReason::unit_not_owned;
                }), "WAB-51: a rejected foreign Stop leaves Hunt running");
            }
            if (tick == 0) {
                const auto units = session.units();
                const auto container = std::find_if(units.begin(), units.end(), [](const auto& unit) { return unit.entity_id == 10; });
                expect(container != units.end() && container->order.kind == tactical::OrderKind::attack_move,
                    "WAB-55: hunt sends the container on an attack-move");
                expect(a && a->slots[0].expires_tick == 0 && a->slots[0].ready_tick == 0 && !a->slots[0].autofire,
                    "WAB-50: no duration, recharge or autofire");
            }
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "hunt hashes are equal for 1/2/4/8 workers");
        const auto encoded = tactical::write_replay(session.record());
        expect(static_cast<bool>(encoded), "HUNT uses the existing ability command encoding");
        if (!encoded) continue;
        const auto parsed = tactical::parse_replay(encoded.value());
        expect(parsed && parsed.value() == session.record(), "HUNT command bytes round trip");
        if (!parsed) continue;
        auto playback = tactical::TacticalSession::from_replay(parsed.value(), {}, {}, hangar_motion(), std::nullopt, {}, {}, table);
        expect(static_cast<bool>(playback), "HUNT opcode 8 round trip creates a session");
        if (playback) {
            for (std::size_t tick = 0; tick < hashes.size(); ++tick) {
                auto step = playback.value().step(executor);
                expect(step && step.value().state_sha256 == hashes[tick], "HUNT replay matches every live frame");
            }
        }
    }
}

void test_projectile_defence_producers() {
    namespace detail = tactical::detail;
    using Kind = tactical::AbilityKind;
    expect(tactical::ability_kind("MISSILE_SHIELD") == Kind::missile_shield, "WPJ-17: shield kind binds");
    expect(tactical::ability_kind("sensor_jamming") == Kind::sensor_jamming, "WHE-32: jammer kind binds");
    tactical::CombatProfile profile; profile.type_id = 1;
    tactical::CombatProfile passive = profile; passive.passive_missile_shield_radius = units(7);
    passive.ranged_target_z_adjust = units(2);
    tactical::CombatState combat;
    tactical::DurabilityState dead; dead.hull = {};
    tactical::MotionTable motion; tactical::MotionProfile mover; mover.type_id = 1;
    motion.profiles = {mover};
    const std::vector<tactical::SnapshotPlayer> players{{1, 1, false}, {2, 1, false}, {3, 3, false}, {4, 4, true}};
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        detail::CombatWorld world; world.relationships = players; world.motion = &motion;
        world.units.resize(10);
        for (std::size_t slot = 0; slot < world.units.size(); ++slot) {
            auto& unit = world.units[slot]; unit.id = slot + 1; unit.type_id = 1;
            unit.owner = 1; unit.profile = &profile; unit.combat = &combat;
        }
        world.units[0].profile = &passive;
        world.units[3].owner = 2; // another player on the same team
        world.units[4].owner = 4; // neutral, not allied
        world.units[5].owner = 3; // enemy
        world.units[6].position = at(0, 0, 11); // planar inclusion is insufficient
        world.units[7].combat = nullptr;
        world.units[8].type_id = 2; // no locomotor
        world.units[9].durability = &dead;
        world.units[3].position = at(0, 0, 10); // inclusive spatial boundary
        std::vector<detail::ProjectileDefenceAbilityInput> inputs(10);
        inputs[1] = {true, false, units(8), {}};
        inputs[2] = {false, true, {}, units(10)};
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto prepared = detail::prepare_projectile_defences(world, inputs, executor, std::vector<eawr::sim::EntityId>{3, 2, 1});
        expect(static_cast<bool>(prepared), "WPJ-17/WHE-32: production copied inputs prepare");
        if (!prepared) continue;
        const auto& sources = world.projectile_defences.sources;
        expect(sources.size() == 3 && sources[0].id == 3 && sources[1].id == 2 && sources[2].id == 1,
            "WPJ-17: source registration order survives spatial indexing");
        expect(world.units[0].sensor_jammed && world.units[2].sensor_jammed && world.units[3].sensor_jammed
            && world.units[4].sensor_jammed, "WHE-32: self, ally and nonenemy neutral receive inclusive 3D stamps");
        for (std::size_t slot = 5; slot < 10; ++slot)
            expect(!world.units[slot].sensor_jammed, "WHE-32: enemy, height, no combat, no locomotor and dead gates");
        tactical::CraftProfile craft; craft.type_id = 2;
        motion.squadrons.craft = {craft};
        prepared = detail::prepare_projectile_defences(world, inputs, executor, std::vector<eawr::sim::EntityId>{3, 2, 1});
        expect(prepared && world.units[8].sensor_jammed, "WHE-32: craft locomotor recipients are included");
        motion.squadrons.craft.clear();
        expect(sources.size() == 3, "WHE-32: ordinary recipients are stamped without registering as emitters");
        expect(sources.size() == 3 && sources[2].adjusted_position.z == units(2), "WPJ-17: copied source uses adjusted height");
        const auto previous = sources;
        inputs[1].active_shield = false;
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && world.projectile_defences.sources.size() == 3
            && !world.projectile_defences.sources[1].active_shield && world.projectile_defences.sources[1].sensor_jammed,
            "WPJ-17: an inactive declared shield remains eligible while externally jammed");
        inputs[2].active_jamming = false;
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && world.projectile_defences.sources.size() == 1
            && world.projectile_defences.sources[0].id == 1, "WPJ-17: deactivation preserves passive source");
        expect(std::none_of(world.units.begin(), world.units.end(), [](const auto& unit) { return unit.sensor_jammed; }),
            "WHE-32: deactivation clears every current-frame stamp");
        expect(previous.size() == 3 && previous[0].sensor_jammed && previous[1].active_shield,
            "WPJ-17: published records are immutable copies of activation state");
        inputs[2].active_jamming = true; world.units[2].in_nebula = true;
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && !world.units[0].sensor_jammed && !world.units[2].sensor_jammed,
            "WHE-31: nebula suspends stamping");
        world.units[2].in_nebula = false; world.units[2].durability = &dead;
        world.units[0].durability = &dead;
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && world.projectile_defences.sources.empty() && !world.units[3].sensor_jammed,
            "WPJ-17/WHE-32: source death removes registration and stamps");
        world.units[0].durability = nullptr; world.units[2].durability = nullptr;
        inputs[2].jamming_radius.reset(); world.units[2].profile = &passive;
        world.units[3].position = at(0, 0, 7);
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && world.units[3].sensor_jammed, "WHE-32: absent jamming radius falls back to passive radius");
        world.units[3].position = at(0, 0, 8);
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && !world.units[3].sensor_jammed, "WHE-32: fallback radius excludes beyond-boundary recipient");
    }
    tactical::AbilityProfile jammer; jammer.kind = Kind::sensor_jamming;
    jammer.expiration_frames = 3; jammer.recharge_frames = 6;
    auto profile_of = unit_of(1, jammer);
    auto state = tactical::initial_abilities(profile_of);
    expect(tactical::activate_ability(jammer, state.slots[0], {}, 10).changed, "WHE-31: existing activation slot starts jamming");
    static_cast<void>(tactical::expire_abilities(profile_of, state, 12));
    expect(state.slots[0].active, "WHE-31: timer stays active before authored expiry boundary");
    static_cast<void>(tactical::expire_abilities(profile_of, state, 13));
    expect(!state.slots[0].active && state.slots[0].ready_tick == 19, "WHE-31: exact expiry starts existing recharge");
    auto invalid = tactical::AbilityTable{}; jammer.effective_radius = units(-1);
    invalid.profiles = {unit_of(1, jammer)};
    expect(!tactical::validate_abilities(invalid), "WPJ-17: invalid authored defence radius is refused");
}

void test_projectile_defence_transaction() {
    using Kind = tactical::AbilityKind;
    const auto order_of = [](const tactical::TacticalSession& session) {
        const auto bytes = session.canonical_state_bytes();
        const std::vector<std::uint8_t> tag{'P', 'D', 'E', 'F'};
        auto position = std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end());
        std::vector<eawr::sim::EntityId> order;
        if (position == bytes.end()) return order;
        auto offset = static_cast<std::size_t>(position - bytes.begin()) + 4;
        const auto read = [&]() {
            std::uint64_t value = 0;
            for (unsigned shift = 0; shift < 64 && offset < bytes.size(); shift += 8)
                value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
            return value;
        };
        const auto count = read();
        if (count > 16) { expect(false, "PDEF test block has bounded source count"); return order; }
        for (std::uint64_t i = 0; i < count; ++i) order.push_back(read());
        return order;
    };
    class FailVisibility final : public eawr::sim::PartitionExecutor {
    public:
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            return eawr::sim::InlineExecutor{}.execute(count, partition);
        }
        eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            if (phase == "visibility") return eawr::core::Result<void>::failure({});
            return execute(count, partition);
        }
    };
    tactical::AbilityProfile jammer; jammer.kind = Kind::sensor_jamming;
    jammer.effective_radius = units(100); jammer.expiration_frames = 3;
    tactical::AbilityProfile shield; shield.kind = Kind::missile_shield; shield.effective_radius = units(100);
    tactical::AbilityTable abilities; abilities.profiles = {unit_of(corvette_type, jammer),
        unit_of(frigate_type, shield), unit_of(plain_type, jammer)};
    tactical::CombatTable combat;
    for (const auto type : {corvette_type, frigate_type, plain_type}) {
        tactical::CombatProfile profile; profile.type_id = type; combat.profiles.push_back(profile);
    }
    auto initial = setup();
    initial.units.push_back({4, plain_type, 1, at(0, 0), math::identity_quat(), {}});
    const std::vector<tactical::PlayerCommand> commands{
        command(0, 1, 0, 2, tactical::AbilityPayload{Kind::missile_shield, tactical::AbilityAction::activate}),
        command(0, 1, 1, 1, tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::activate}),
        command(1, 1, 0, 4, tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::activate}),
        command(2, 1, 0, 1, tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::deactivate}),
        command(2, 1, 1, 1, tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::activate}),
        command(3, 1, 0, 1, tactical::DamagePayload{units(100000), tactical::hull_target})};
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(initial, {}, durability(), motion(), std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(created), "PDEF lifecycle session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        for (const auto& item : commands) expect(static_cast<bool>(session.submit(item)), "PDEF lifecycle command submits");
        expect(order_of(session).empty(), "PDEF is absent before any jammer activation");
        const auto before = session.canonical_state_bytes();
        expect(!session.step(FailVisibility{}), "PDEF failed frame reaches post-command failure seam");
        expect(session.canonical_state_bytes() == before && session.completed_tick() == 0,
            "PDEF failed frame rolls back registration and ability activation");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const std::vector<std::vector<eawr::sim::EntityId>> orders{{2, 1}, {2, 1, 4}, {2, 4, 1}, {2, 4}, {}, {}};
        for (std::size_t frame = 0; frame < orders.size(); ++frame) {
            session.scramble_storage_for_testing();
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "PDEF lifecycle frame succeeds after retry");
            if (!step) break;
            hashes.push_back(step.value().state_sha256);
            expect(order_of(session) == orders[frame], "PDEF retains activation order, appends reactivation, prunes death/expiry");
        }
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "PDEF transaction hashes equal at 1/2/4/8 workers");
        const auto encoded = tactical::write_replay(session.record());
        const auto parsed = encoded ? tactical::parse_replay(encoded.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(parsed && parsed.value() == session.record(), "WPJ-17/WHE-32: appended kinds round trip through opcode 8");
        if (!parsed) continue;
        auto playback = tactical::TacticalSession::from_replay(parsed.value(), {}, durability(), motion(), std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(playback), "PDEF lifecycle replay creates");
        if (playback) for (const auto& hash : hashes) {
            const auto step = playback.value().step(executor);
            expect(step && step.value().state_sha256 == hash, "PDEF replay reproduces source ordering at every frame");
        }
    }
    // Creation registers a declared shield immediately, even while inactive.
    // Exercise both orders in the command batch, including a post-command rollback.
    for (const bool reinforce_first : {true, false}) {
        std::vector<std::string> creation_reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto creation_setup = setup();
            tactical::EconomyRules economy;
            economy.players = {{1, units(100), 25, false, {}, 1, 3}};
            tactical::BuildOption option;
            option.type = frigate_type; option.price = units(10);
            option.build_frames = 1; option.ai_build_frames = 1; option.available = true;
            economy.menus = {{corvette_type, 1, {option}}};
            economy.footprints = {{frigate_type, std::nullopt, {}}};
            auto created = tactical::TacticalSession::create(creation_setup, {}, durability(), motion(),
                std::nullopt, combat, {}, abilities, economy);
            expect(static_cast<bool>(created), "PDEF reinforcement ordering session creates");
            if (!created) continue;
            auto session = std::move(created).value();
            expect(static_cast<bool>(session.submit(command(0, 1, 0, 1, tactical::BuyPayload{frigate_type}))),
                "PDEF reinforcement purchase submits");
            const auto activate_sequence = reinforce_first ? 1U : 0U;
            const auto reinforce_sequence = reinforce_first ? 0U : 1U;
            std::vector<tactical::PlayerCommand> batch{
                command(2, 1, activate_sequence, 1,
                    tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::activate}),
                {{2, 1, reinforce_sequence}, {}, tactical::ReinforcePayload{frigate_type, at(0, 0)}}};
            std::sort(batch.begin(), batch.end(), [](const auto& a, const auto& b) {
                return a.key.sequence < b.key.sequence;
            });
            for (const auto& item : batch) expect(static_cast<bool>(session.submit(item)),
                "PDEF same-frame creation commands submit in sequence order");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 3; ++frame) {
                if (frame == 2) {
                    const auto before = session.canonical_state_bytes();
                    expect(!session.step(FailVisibility{}), "PDEF creation frame reaches failure seam");
                    expect(session.canonical_state_bytes() == before && session.units().size() == 3,
                        "PDEF creation rollback restores ledger, entity allocation and reinforcement pool");
                }
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), "PDEF creation frame succeeds after retry");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
            }
            expect(session.units().size() == 4 && session.arrivals().size() == 1,
                "PDEF fixture creates one reinforcement in the activation frame");
            const std::vector<eawr::sim::EntityId> expected = reinforce_first
                ? std::vector<eawr::sim::EntityId>{2, 4, 1} : std::vector<eawr::sim::EntityId>{2, 1, 4};
            expect(order_of(session) == expected,
                "PDEF static creation registers at its command position before or after jammer activation");
            if (workers == 1) creation_reference = hashes;
            else expect(hashes == creation_reference, "PDEF creation order hashes equal at 1/2/4/8 workers");
        }
    }
}

} // namespace

int main() {
    test_projectile_defence_producers();
    test_projectile_defence_transaction();
    test_impact_shooter_bonus();
    test_hero_environment_damage_routing();
    test_hero_wingmen();
    test_spawned_hero_abilities();
    test_hero_beams();
    test_hunt_destinations();
    test_hunt_ship_order();
    test_hunt_session();
    test_names_and_validation();
    test_hero_damage_modes();
    test_nested_special_handlers();
    test_concentrate_fire();
    test_concentrate_damage();
    test_concentrate_sparse_lifecycle();
    test_timers();
    test_multipliers();
    test_turbo_session();
    test_expiry_snapshot_keeps_timed_duration();
    test_defend_stand_in();
    test_autofire_rules();
    test_rejections();
    test_launched_squadron_sfoils();
    test_created_squadron_autofire();
    test_barrage_proxy_and_override();
    if (failures != 0) {
        std::cerr << failures << " ability contract failure(s)\n";
        return 1;
    }
    std::cout << "ability contracts passed\n";
    return 0;
}
