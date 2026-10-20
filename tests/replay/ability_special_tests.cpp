#include "ability_fixtures.hpp"
#include "../../src/sim/tactical/session_store_abilities.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ability_tests {

namespace {


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

} // namespace


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
    for (const unsigned scenario : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}) {
        const bool speed_case = scenario >= 6;
        std::vector<std::string> baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
            tactical::AbilityProfile ordinary;
            ordinary.kind = tactical::AbilityKind::concentrate_fire;
            ordinary.expiration_frames = 100;
            if (scenario == 6) ordinary.expiration_frames = 8;
            ordinary.recharge_frames = 20;
            ordinary.effective_radius = units(6000);
            ordinary.gui_activated_ability_name = "focus";
            auto hero = unit_of(plain_type, ordinary);
            tactical::SpecialAbilityProfile special;
            special.name = "focus"; special.kind = tactical::SpecialAbilityKind::concentrate_fire;
            special.style = tactical::SpecialActivationStyle::user_input; special.service_interval = 1;
            special.target_damage_increase = decimal("0.5"); special.filter.applicable_categories = 2;
            if (speed_case) special.target_speed_decrease = decimal("0.4");
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
            if (scenario == 4 || scenario == 5) {
                tactical::CommandBonusProfile bonus;
                bonus.type = frigate_type; bonus.apply_to_self = true;
                bonus.bonus.applicable = {frigate_type};
                bonus.bonus.stacking_category = scenario == 4 ? 0 : 1;
                bonus.bonus.percentages[4] = decimal("0.25");
                economy.command_bonuses = {bonus};
            }
            auto movement = speed_case ? motion() : tactical::MotionTable{};
            for (auto& row : movement.profiles) { row.acceleration = row.max_speed; row.deceleration = row.max_speed; }
            auto created = tactical::TacticalSession::create(initial,
                std::vector<tactical::SensorProfile>{{corvette_type, units(1000)}, {frigate_type, units(1000)}},
                health, movement, std::nullopt, combat, {}, abilities, economy);
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
            if (scenario == 7) expect(static_cast<bool>(session.submit(command(8, 1, 3, 3,
                tactical::AbilityPayload{ordinary.kind, tactical::AbilityAction::deactivate}))), "speed effect releases before impact");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            if (speed_case) expect(static_cast<bool>(session.submit(command(0, 2, 0, 2,
                tactical::MovePayload{at(6000, 0)}))), "WHE-25: target moves through concentrate-fire speed changes");
            std::vector<std::string> hashes;
            unsigned hits = 0;
            for (unsigned frame = 0; frame < 30; ++frame) {
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), "concentrate-fire damage step succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
                if (speed_case && (frame == 3 || frame == 12)) {
                    const auto* profile = movement.find(frigate_type);
                    const auto state = session.motion_state(2);
                    const auto sampled = state ? tactical::sample_motion(*state, session.completed_tick(),
                        state->start_position, state->start_yaw) : eawr::core::Result<tactical::MotionSample>::failure({});
                    const auto factor = frame == 3 ? Fixed::from_raw(one - decimal("0.4").raw()) : units(1);
                    const auto expected_speed = profile ? math::multiply(profile->max_speed, factor)
                        : eawr::core::Result<Fixed>::failure({});
                    expect(sampled && expected_speed && sampled.value().speed == expected_speed.value(),
                        "WHE-25: target slows by the authored fraction and regains speed when the effect expires; frame="
                            + std::to_string(frame) + ", scenario=" + std::to_string(scenario)
                            + ", expected=" + std::to_string(expected_speed ? expected_speed.value().raw() : -1)
                            + ", sampled=" + std::to_string(sampled ? sampled.value().speed.raw() : -1));
                }
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


// B2 of the determinism audit: one source applying two kinds to one target in one stacking category tied on
// the old (target, category, source) key, so the stored order followed the input order. The key is now the
// whole record; both input orders must give the same vector and the same changed-target set.
void test_concentrate_effect_total_order() {
    using Store = tactical::session_detail::SessionStoreAbilityEffects;
    using Effect = Store::ConcentrateEffect;
    const Effect tractor{7, 5, 1, {}, units(-2), tactical::AbilityKind::tractor_beam, {}};
    const Effect focus{7, 5, 1, units(-1), units(-3), tactical::AbilityKind::concentrate_fire, {}};
    const Effect weaken{7, 5, 1, units(-4), {}, tactical::AbilityKind::weaken_enemy, units(-5)};
    const Effect other{3, 9, 0, units(-1), {}, tactical::AbilityKind::concentrate_fire, {}};
    const Effect stale{11, 2, 0, {}, units(-1), tactical::AbilityKind::tractor_beam, {}};
    const std::vector<Effect> forward{tractor, focus, weaken, other};
    std::vector<Effect> backward(forward.rbegin(), forward.rend());
    auto rotated = forward;
    std::rotate(rotated.begin(), rotated.begin() + 2, rotated.end());

    std::vector<Effect> sorted_forward = forward, sorted_backward = backward, sorted_rotated = rotated;
    Store::sort_concentrate_effects(sorted_forward);
    Store::sort_concentrate_effects(sorted_backward);
    Store::sort_concentrate_effects(sorted_rotated);
    expect(sorted_forward == sorted_backward && sorted_forward == sorted_rotated,
        "concentrate effects tying on (target, category, source) sort identically from any input order");
    expect(sorted_forward.size() == 4 && sorted_forward.front() == other,
        "concentrate effects keep every tied record and order by target first");

    Store left, right;
    left.concentrate_effects = {stale};
    right.concentrate_effects = {stale};
    left.commit_concentrate_effects(forward);
    right.commit_concentrate_effects(backward);
    expect(left.concentrate_effects == right.concentrate_effects,
        "concentrate commit stores the same vector from either input order");
    expect(left.concentrate_changed_targets == right.concentrate_changed_targets
        && left.concentrate_changed_targets == std::set<eawr::sim::EntityId>{3, 7, 11},
        "concentrate commit marks the same changed targets from either input order");

    // Recommitting the same effects in the other order is not a change.
    left.concentrate_changed_targets.clear();
    left.commit_concentrate_effects(backward);
    expect(left.concentrate_effects == sorted_forward && left.concentrate_changed_targets.empty(),
        "concentrate commit of an equal set in another order changes nothing");
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


void test_squadron_reveal_ping() {
    for (const bool blocked : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup initial;
            initial.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
            initial.units = {{2, xwing_type, 1, at(0, 0), math::identity_quat(), {}},
                {3, xwing_type, 1, at(-10, 10), math::identity_quat(), {}},
                {10, xwing_squadron, 1, at(0, 0), math::identity_quat(), {}}};
            initial.squadrons = {{10, {2, 3}}};
            tactical::AbilityProfile ability;
            ability.kind = tactical::AbilityKind::fow_reveal_ping;
            ability.recharge_frames = 900;
            ability.spawned = tactical::SpawnedAbilityProfile{};
            ability.spawned->type = 99;
            ability.spawned->damage_type = 0;
            ability.spawned->shield_damage = false;
            ability.spawned->reveal_range = units(1000);
            ability.spawned->hitpoint_damage = false;
            tactical::AbilityTable table; table.profiles = {unit_of(xwing_type, ability)};
            if (!blocked && workers == 1) {
                auto no_reveal = table;
                no_reveal.profiles.front().abilities.front().spawned->reveal_range.reset();
                expect(static_cast<bool>(tactical::validate_abilities(no_reveal)),
                    "WFP-02: an authored spawned object without REVEAL does not refuse the ability table");
            }
            auto health = hangar_durability(); health.damage = durability().damage;
            for (auto& profile : health.profiles) { profile.armor_type = 0; profile.shield_armor_type = 0; }
            tactical::FogRules fog; fog.map_left = units(-2000); fog.map_top = units(2000);
            fog.cell_size = units(100); fog.cells_wide = 80; fog.cells_tall = 40;
            fog.service_period = 16; fog.ramp_down_step = 21;
            tactical::CombatTable combat;
            tactical::CombatProfile box; box.type_id = xwing_type;
            box.collision = tactical::CollisionBox{at(-1, -1, -1), at(1, 1, 1)};
            combat.profiles = {box};
            auto created = tactical::TacticalSession::create(initial, {}, health, hangar_motion(), fog, combat, {}, table);
            expect(static_cast<bool>(created), "AB-15/WFP: squadron ping session creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            const tactical::AbilityPayload ping{tactical::AbilityKind::fow_reveal_ping,
                tactical::AbilityAction::activate, 0, tactical::no_hardpoint, at(3000, 0)};
            expect(static_cast<bool>(world.submit(command(0, 1, 0, blocked ? 2 : 10, ping))), "AB-15/WFP: first ping queues");
            expect(static_cast<bool>(world.submit(command(1, 1, 1, 10, ping))), "AB-15/WFP: squadron repeat queues");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 211; ++frame) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "AB-15/WFP: squadron ping steps");
                if (!step) { std::cerr << "frame " << frame << ": " << step.error().message << '\n'; break; }
                hashes.push_back(step.value().state_sha256);
                if (frame == 0) {
                    expect(world.projectiles().size() == (blocked ? 1U : 2U), "AB-15/WFP: one ping per acting craft");
                    expect(world.ability_state(2)->slots[0].ready_tick == 900
                        && world.ability_state(3)->slots[0].ready_tick == (blocked ? 0U : 900U),
                        "AB-15/WFP: recharge belongs to acting craft, not container");
                    expect(world.fog_cells()->revealed(0, at(3000, 0)) && !world.fog_cells()->revealed(1, at(3000, 0)),
                        "WFP-01/02: squadron point reveals only to its owner");
                }
                if (frame == 1) {
                    expect(!step.value().diagnostics.empty(), "AB-15/WFP: any unready holder rejects the whole squadron");
                    expect(world.ability_state(3)->slots[0].ready_tick == (blocked ? 0U : 900U),
                        "AB-15/WFP: rejected squadron cannot partially recharge or spawn");
                    const auto terminals = std::count_if(step.value().snapshot->combat_events().begin(),
                        step.value().snapshot->combat_events().end(), [](const auto& event) {
                            return event.kind == tactical::CombatEventKind::projectile_expired && event.aim == at(3000, 0);
                        });
                    expect(terminals == (blocked ? 1 : 2), "WFP-03/WPJ-35: each craft ping publishes one point terminal");
                }
                if (frame >= 1) expect(world.projectiles().empty(), "WFP-03: squadron pings expire at first flight service");
                expect(world.fog_cells()->revealed(0, at(3000, 0)) == (frame < 209), "V-15: overlapping craft reveals regrow normally");
            }
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "AB-15/WFP: squadron ping hashes equal on 1/2/4/8 workers");
            const auto bytes = tactical::write_replay(world.record());
            const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == world.record(), "AB-15/WFP: squadron point replay round-trips");
        }
    }
}


void test_cluster_bomb_and_reveal_ping() {
    using K = tactical::AbilityKind;
    for (const auto kind : {K::cluster_bomb, K::fow_reveal_ping}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}}};
            tactical::AbilityProfile ability;
            ability.kind = kind; ability.recharge_frames = kind == K::cluster_bomb ? 1500 : 900;
            ability.supports_autofire = kind == K::cluster_bomb;
            ability.spawned = tactical::SpawnedAbilityProfile{};
            auto& spawn = *ability.spawned;
            spawn.type = 99; spawn.damage_type = 0; spawn.shield_damage = false;
            if (kind == K::cluster_bomb) {
                spawn.ring_count = 12; spawn.countdown_frames = 45; spawn.speed = units(12); spawn.max_travel = units(1000);
                spawn.blast.damage = units(150); spawn.blast.radius = units(150);
            } else {
                spawn.reveal_range = units(1000); spawn.hitpoint_damage = false;
                // WFP-03: stock motionless, zero-distance profile has no authored lifetime.
            }
            tactical::AbilityTable table; table.profiles = {unit_of(plain_type, ability)};
            tactical::FogRules fog; fog.map_left = units(-2000); fog.map_top = units(2000);
            fog.cell_size = units(100); fog.cells_wide = 80; fog.cells_tall = 40;
            fog.service_period = kind == K::fow_reveal_ping ? 16 : 1;
            fog.ramp_down_step = kind == K::fow_reveal_ping ? 21 : 238;
            tactical::CombatTable combat;
            tactical::CombatProfile collision; collision.type_id = plain_type;
            collision.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
            combat.profiles = {collision};
            auto created = tactical::TacticalSession::create(initial, {}, durability(), {}, fog, combat, {}, table);
            expect(static_cast<bool>(created), "WCB/WFP: session creates");
            if (!created) continue;
            auto world = std::move(created).value();
            tactical::AbilityPayload input{kind, tactical::AbilityAction::activate};
            if (kind == K::fow_reveal_ping) input.position = at(3000, 0);
            expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, input))), "WCB/WFP: activation queues");
            expect(static_cast<bool>(world.submit(command(1, 1, 1, 1, input))), "WCB/WFP: recharge use queues");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < (kind == K::fow_reveal_ping ? 211U : 47U); ++frame) {
                const auto stepped = world.step(executor);
                expect(static_cast<bool>(stepped), "WCB/WFP: step succeeds");
                if (!stepped) { std::cerr << stepped.error().message << '\n'; break; }
                hashes.push_back(stepped.value().state_sha256);
                if (frame == 0) {
                    const auto projectiles = world.projectiles();
                    expect(projectiles.size() == (kind == K::cluster_bomb ? 12U : 1U), "WCB/WFP: exact spawn count");
                    expect(world.ability_state(1)->slots[0].ready_tick == ability.recharge_frames
                        && !world.ability_state(1)->slots[0].active, "WCB/WFP: instant use starts full recharge");
                    if (kind == K::cluster_bomb && projectiles.size() == 12) {
                        for (std::size_t i = 0; i < projectiles.size(); ++i) {
                            expect(projectiles[i].id == i + 1 && projectiles[i].position == at(0, 0), "WCB-02: ordered launch at ship");
                            expect(projectiles[i].yaw == units(static_cast<std::int64_t>(i) * 30), "WCB-02: twelve 30-degree headings");
                        }
                    } else if (kind == K::fow_reveal_ping) {
                        expect(world.fog_cells()->revealed(0, at(3000, 0)), "WFP-01/02: unlimited click reveals for owner");
                        expect(!world.fog_cells()->revealed(1, at(3000, 0)), "WFP-01: enemy receives no reveal");
                        expect(!world.fog_cells()->revealed(0, at(4500, 0)), "WFP-01: reveal uses projectile radius");
                    }
                }
                if (frame == 1) {
                    expect(std::any_of(stepped.value().snapshot->events().begin(), stepped.value().snapshot->events().end(),
                        [](const auto& event) { return event.kind == tactical::EventKind::order_rejected; }), "WCB/WFP: recharge rejects repeat");
                    if (kind == K::cluster_bomb) expect(!world.projectiles().empty() && world.projectiles().front().position != at(0, 0), "WCB-02: bombs really fly");
                    else {
                        expect(world.projectiles().empty(), "WFP-03: stock ping expires on its first flight service");
                        const auto terminals = std::count_if(stepped.value().snapshot->combat_events().begin(), stepped.value().snapshot->combat_events().end(),
                            [](const auto& event) { return event.kind == tactical::CombatEventKind::projectile_expired
                                && event.aim == at(3000, 0); });
                        expect(terminals == 1, "WFP-03/WPJ-35: ping publishes one lifetime terminal at the clicked point");
                    }
                }
                if (kind == K::cluster_bomb && frame == 44) {
                    expect(world.projectiles().size() == 12, "WCB-02: bombs wait 45 frames before countdown detonation");
                }
                if (kind == K::cluster_bomb && frame == 45) expect(world.projectiles().empty(), "WCB-02: countdown detonates ring");
                if (kind == K::fow_reveal_ping && frame >= 1) {
                    expect(world.projectiles().empty(), "WFP-03: ping never returns after expiry");
                    // V-15: player 1's expiry-frame service precedes source release.
                    // First decay at 17 gives 238; the thirteenth at 209 gives 0.
                    expect(world.fog_cells()->revealed(0, at(3000, 0)) == (frame < 209),
                        "WFP-03/V-15: stock cadence keeps the released area revealed for 208 ticks");
                }
            }
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "WCB/WFP: 1/2/4/8 workers equal");
            const auto encoded = tactical::write_replay(world.record());
            const auto parsed = encoded ? tactical::parse_replay(encoded.value())
                : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == world.record(), "WCB/WFP: new kinds and position commands round-trip");
            if (parsed) {
                auto replay = tactical::TacticalSession::from_replay(parsed.value(), {}, durability(), {}, fog, combat, {}, table);
                expect(static_cast<bool>(replay), "WCB/WFP: replay binds");
                if (replay) {
                    std::vector<std::string> replay_hashes;
                    while (replay.value().completed_tick() < parsed.value().final_tick_count) {
                        const auto step = replay.value().step(executor);
                        if (!step) break;
                        replay_hashes.push_back(step.value().state_sha256);
                    }
                    expect(replay_hashes == hashes, "WCB/WFP: replay reproduces projectile and fog state");
                }
            }
        }
    }
}


// FSC-45/46: the previous sample, strict clock admission, and human state transition.
void test_mc30_object_script() {
    using K = tactical::AbilityKind;
    for (const bool human : {false, true}) for (const bool control : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{30, plain_type, 1, at(0, 0), math::identity_quat(), {}},
                {31, frigate_type, 2, at(200, 0), math::identity_quat(), {}},
                {32, frigate_type, 2, at(200, 100), math::identity_quat(), {}},
                {33, frigate_type, 2, at(200, -100), math::identity_quat(), {}}};
            if (!human) initial.units.resize(2);
            tactical::AbilityProfile ability;
            ability.kind = K::cluster_bomb; ability.recharge_frames = 1500; ability.supports_autofire = true;
            ability.spawned = tactical::SpawnedAbilityProfile{};
            auto& spawn = *ability.spawned;
            spawn.type = 99; spawn.ring_count = 12; spawn.countdown_frames = 45;
            spawn.speed = units(12); spawn.max_travel = units(1000);
            auto profile = unit_of(plain_type, ability); profile.mc30_script = true;
            tactical::AbilityTable table; table.profiles = {profile};
            table.combat_ratings = {{frigate_type, units(control && !human ? 999 : 1000)}};
            if (human) table.humans = {1};
            tactical::CombatTable combat;
            for (const auto type : {frigate_type, plain_type}) {
                tactical::CombatProfile collision; collision.type_id = type;
                collision.collision = tactical::CollisionBox{at(-1, -1, -1), at(1, 1, 1)};
                combat.profiles.push_back(collision);
            }
            auto movement = motion();
            for (auto& row : movement.profiles) {
                row.max_speed = units(100); row.acceleration = units(100); row.deceleration = units(100);
                row.rate_of_turn = units(360);
            }
            auto created = tactical::TacticalSession::create(initial, {}, durability(), movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "FSC-45/46: MC30 script session creates");
            if (!created) continue;
            auto world = std::move(created).value();
            if (human) {
                expect(static_cast<bool>(world.submit(command(30, 1, 0, 30,
                    tactical::AbilityPayload{K::cluster_bomb, tactical::AbilityAction::autofire_on}))), "FSC-46: arm before due turn");
                if (control) expect(static_cast<bool>(world.submit(command(32, 1, 1, 30,
                    tactical::AbilityPayload{K::cluster_bomb, tactical::AbilityAction::autofire_off}))), "FSC-46: disarm before next due turn");
            } else {
                expect(static_cast<bool>(world.submit(command(1, 2, 0, 31, tactical::MovePayload{at(2000, 0)}))),
                    "FSC-45: sampled enemy leaves before due update");
            }
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 64; ++frame) {
                const auto stepped = world.step(executor);
                expect(static_cast<bool>(stepped), "FSC-45/46: step succeeds");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                const auto state = world.ability_state(30);
                expect(state && state->mc30_script, "FC-57: first pump initializes hashed script");
                if (!state || !state->mc30_script) break;
                const auto& script = *state->mc30_script;
                if (frame == 0) expect(state->slots[0].ready_tick == 0 && script.nearby == (human ? 3U : 1U),
                    "FD-47: first empty update does not fire; wrapper builds sample");
                if (frame == 30) expect(script.last_service == 0 && state->slots[0].ready_tick == 0,
                    "FSC-45: exactly one second skips and retains previous sample");
                if (frame == 31) {
                    expect(script.last_service > (std::int64_t{1} << 28), "FC-57: frame 31 admits update");
                    expect(state->slots[0].ready_tick == (!human && !control ? 1531U : 0U),
                        "FSC-45/46: old AI threat fires; arming clears human sample");
                    expect(world.projectiles().size() == (!human && !control ? 12U : 0U),
                        "FD-47: twelve bombs created directly in the admitted turn");
                    expect(script.nearby == (human ? 3U : 0U), "FD-47: new wrapper sample follows update");
                    if (!human && !control) for (const auto& projectile : world.projectiles())
                        expect(projectile.position == at(0, 0), "FD-47: creation precedes first flight service");
                }
                if (frame == 32) {
                    expect(script.last_service > (std::int64_t{1} << 28) && script.last_service < (std::int64_t{2} << 28),
                        "FC-57: one frame since last service skips");
                    if (!human && !control) expect(!world.projectiles().empty() && world.projectiles().front().position != at(0, 0),
                        "FD-47: record next-frame first projectile flight separately");
                }
                if (frame == 32 && human && control) expect(script.armed && script.nearby == 3 && !state->slots[0].autofire,
                    "FSC-46: disarming on an unadmitted frame retains the script sample and state");
                // FC-57: rounding makes this interval 30 frames; it is not a fixed 31-frame deadline.
                if (frame == 61 && human) {
                    expect(state->slots[0].ready_tick == (control ? 0U : 1561U), "FSC-46: staying armed uses retained count; disarming avoids use");
                    expect(world.projectiles().size() == (control ? 0U : 12U), "FSC-46: direct human activation at next due pump");
                }
            }
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "FSC-45/46: 1/2/4/8 worker hashes equal");
            auto playback = tactical::TacticalSession::from_replay(world.record(), {}, durability(), movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(playback), "MSCR: object-script replay creates");
            if (playback) {
                std::vector<std::string> replay_hashes;
                while (playback.value().completed_tick() < world.completed_tick()) {
                    const auto step = playback.value().step(executor);
                    if (!step) break;
                    replay_hashes.push_back(step.value().state_sha256);
                }
                expect(replay_hashes == hashes, "MSCR: retained samples and stamps replay exactly");
            }
        }
    }
}


void test_mc30_proximity_sample() {
    using K = tactical::AbilityKind;
    // FC-58: lower distance uses promoted position; two collected craft count once.
    for (const bool squadron : {false, true}) for (const auto distance : {149, 150, 501, 502}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{30, plain_type, 1, at(0, 0), math::identity_quat(), {}},
                {31, squadron ? xwing_squadron : frigate_type, 2, at(distance, 0), math::identity_quat(), {}}};
            auto movement = motion();
            auto health = durability();
            tactical::CombatTable combat;
            tactical::CombatProfile box; box.type_id = squadron ? xwing_type : frigate_type;
            box.collision = tactical::CollisionBox{at(-1, -1, -1), at(1, 1, 1)};
            combat.profiles = {box};
            if (squadron) {
                initial.units.push_back({32, xwing_type, 2, at(200, 0), math::identity_quat(), {}});
                initial.units.push_back({33, xwing_type, 2, at(200, 0), math::identity_quat(), {}});
                initial.squadrons = {{31, {32, 33}}};
                movement.squadrons = hangar_motion().squadrons;
                movement.squadrons.spawners.clear();
                // Keep the craft positions where the test stages them; only the collection and promotion matter.
                for (auto& craft : movement.squadrons.craft) { craft.min_speed = {}; craft.max_speed = units(1); }
                const auto extra = hangar_durability();
                for (const auto& row : extra.profiles) if (row.type_id == xwing_type) health.profiles.push_back(row);
                std::sort(health.profiles.begin(), health.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
            }
            tactical::AbilityProfile ability; ability.kind = K::cluster_bomb; ability.recharge_frames = 1500;
            ability.spawned = tactical::SpawnedAbilityProfile{};
            ability.spawned->type = 99; ability.spawned->ring_count = 12; ability.spawned->countdown_frames = 45;
            ability.spawned->speed = units(12);
            auto profile = unit_of(plain_type, ability); profile.mc30_script = true;
            tactical::AbilityTable table; table.profiles = {profile};
            table.combat_ratings = {{squadron ? xwing_squadron : frigate_type, units(1000)}};
            auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "FC-58: proximity session creates");
            if (!created) continue;
            auto world = std::move(created).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "FC-58: sample step succeeds");
            if (!step) continue;
            const auto state = world.ability_state(30);
            const bool eligible = distance >= 150 && (squadron || distance <= 501);
            expect(state && state->mc30_script && state->mc30_script->nearby == (eligible ? 1U : 0U)
                && state->mc30_script->threat == units(eligible ? 1000 : 0),
                "FC-58: exclude below 150, include exactly 150/bounds, deduplicate and rate promoted parent");
            if (reference.empty()) reference.push_back(step.value().state_sha256);
            else expect(reference.front() == step.value().state_sha256, "FC-58: promoted samples equal on 1/2/4/8 workers");
        }
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


// WAB-36, WHE-24 (legacy #2997): CONCENTRATE_FIRE admits an enemy attack target, so a player the
// engine plays switches it on with no command; the command path's recruitment runs (the same-owner
// ally near the target gets the attack). The activation is engine-synthesised: no recorded command
// carries a bit-63 sequence, no order event announces it, and a replay re-derives it.
// Scenarios: 0 AI; 1 human disarmed; 2 human armed; 3 CF then beam; 4 beam then CF;
// 5 CF empty-query release then beam; 6 departed issuer without takeover;
// 7 explicitly disabled CF; 8 explicitly disabled beam.
void test_concentrate_autofire() {
    using K = tactical::AbilityKind;
    for (const unsigned scenario : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}) {
        std::vector<std::string> baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(100, 0, 100), math::identity_quat(), {}},
                {3, frigate_type, 1, at(150, 50), math::identity_quat(), {}}};
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
            tactical::AbilityProfile ordinary;
            ordinary.kind = K::concentrate_fire;
            ordinary.effective_radius = units(scenario == 5 ? 1 : 6000);
            ordinary.expiration_frames = 30;
            ordinary.recharge_frames = 60;
            ordinary.supports_autofire = true;
            ordinary.gui_activated_ability_name = "focus";
            tactical::SpecialAbilityProfile nested;
            nested.name = "focus";
            nested.kind = tactical::SpecialAbilityKind::concentrate_fire;
            nested.style = tactical::SpecialActivationStyle::user_input;
            nested.service_interval = 1;
            nested.filter.applicable_categories = 2;
            nested.target_damage_increase = decimal("0.5");
            auto holder = unit_of(corvette_type, ordinary);
            holder.special = {nested};
            if (scenario == 7 || scenario == 8) holder.special.front().initially_enabled = false;
            if ((scenario >= 3 && scenario <= 6) || scenario == 8) {
                tactical::AbilityProfile beam;
                beam.kind = K::tractor_beam;
                beam.recharge_frames = 20;
                beam.gui_activated_ability_name = "tractor";
                holder.abilities.push_back(beam);
                tactical::SpecialAbilityProfile tractor;
                tractor.name = "tractor";
                tractor.kind = tactical::SpecialAbilityKind::tractor_beam;
                tractor.style = tactical::SpecialActivationStyle::user_input;
                tractor.service_interval = 1;
                tractor.beam_max_range = units(300);
                tractor.filter.applicable_categories = 2;
                if (scenario == 8) tractor.initially_enabled = false;
                holder.special.push_back(tractor);
                if (scenario == 4) std::swap(holder.special[0], holder.special[1]);
            }
            tactical::AbilityTable table; table.profiles = {holder};
            if (scenario == 1 || scenario == 2) table.humans = {1};
            tactical::CombatProfile source; source.type_id = corvette_type; source.category_bits = 1;
            tactical::WeaponProfile weapon;
            weapon.range = units(300); weapon.min_recharge_hundredths = 10; weapon.max_recharge_hundredths = 10;
            weapon.cone_width = units(360); weapon.cone_height = units(180);
            weapon.shot = tactical::ShotProfile{units(1), 0, units(1000), units(1000), true, true, {}};
            source.weapons = {weapon}; source.max_attack_distance = units(300);
            tactical::CombatProfile target; target.type_id = frigate_type; target.category_bits = 2;
            target.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
            tactical::CombatTable combat; combat.profiles = {source, target};
            const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(1000)}, {frigate_type, units(1000)}};
            const auto movement = motion();
            auto created = tactical::TacticalSession::create(initial, sensors, health, movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "WAB-36: concentrate autofire fixture binds");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            if (scenario == 2) expect(static_cast<bool>(world.submit(command(0, 1, 0, 1,
                tactical::AbilityPayload{K::concentrate_fire, tactical::AbilityAction::autofire_on}))), "WAB-36: autofire switches on");
            if (scenario == 6) expect(static_cast<bool>(world.submit({{0, 1, 0}, {}, tactical::QuitPayload{}})),
                "WAB-36: the holder's issuer leaves without takeover");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            std::optional<unsigned> locked;
            bool announced = false;
            std::optional<unsigned> first_switch;
            std::optional<unsigned> first_target;
            std::optional<tactical::TacticalSession> restored;
            for (unsigned frame = 0; frame < (scenario == 6 ? 100U : 8U); ++frame) {
                const auto stepped = world.step(executor);
                expect(static_cast<bool>(stepped), "WAB-36: concentrate autofire tick succeeds");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                for (const auto& event : stepped.value().snapshot->events())
                    announced = announced || (event.sequence & (std::uint64_t{1} << 63)) != 0;
                if (restored) {
                    const auto resumed = restored->step(executor);
                    expect(resumed && resumed.value().state_sha256 == stepped.value().state_sha256,
                        "WAB-36: checkpoint continuation equals uninterrupted holder pass at 1/2/4/8 workers");
                }
                const auto attack_state = world.combat_state(1);
                if (!first_target && attack_state && attack_state->attack_target == 2) first_target = frame;
                const auto state = world.ability_state(1);
                if (scenario >= 3 && scenario <= 6 && state) {
                    const bool focus = state->slots[0].active;
                    const bool beam = state->slots[1].active;
                    if (!first_switch && (focus || beam)) {
                        first_switch = frame;
                        expect(first_target && frame == *first_target,
                            "WAB-36: the command-stage beam or concentrate activation keeps the first eligible tick");
                        expect(focus != beam, "WAB-36: only one handler switches on in the first eligible tick");
                        expect(focus == (scenario == 3 || scenario == 6), "WAB-36: authored order and actual switch-on choose the winner");
                    }
                    if (scenario == 5) expect(!focus, "WHE-24: empty same-owner query releases without consuming the pass");
                    if (scenario == 6) {
                        // WBF-48: quit commits at frame tail; the tick-zero pass may activate CF.
                        // Thereafter the ready beam and, later, the recharged CF remain refused.
                        expect(!beam && (!focus || state->slots[0].started_tick == 0),
                            "WAB-36: departed issuer cannot switch another handler on after the admitted quit tick");
                        expect(stepped.value().diagnostics.empty(), "WAB-36: departed holder is not requeued with rejected commands");
                    }
                }
                if (scenario == 7 || scenario == 8) {
                    expect(state && std::none_of(state->slots.begin(), state->slots.end(),
                        [](const auto& slot) { return slot.active; }),
                        "WAB-36: explicitly disabled CF and beam never switch on");
                    expect(attack_state && attack_state->attack_target == 2,
                        "WAB-36: disabled handlers still have an eligible attack target");
                }
                if (state && state->slots.front().active && !locked) {
                    locked = frame;
                    const auto attack = world.combat_state(1);
                    expect(attack && state->slots.front().target == attack->attack_target && state->slots.front().target == 2,
                        "WAB-36: concentrate fire locks the unit's own attack target");
                    expect(state->concentrate_recruits == std::vector<eawr::sim::EntityId>{3},
                        "WHE-24: the autofired activation recruits the same-owner ally near the target");
                    const auto live_units = world.units();
                    const auto ally = std::find_if(live_units.begin(), live_units.end(), [](const auto& unit) { return unit.entity_id == 3; });
                    expect(ally != live_units.end() && ally->order.kind == tactical::OrderKind::attack && ally->order.target == 2,
                        "WHE-24: the recruited ally takes the ordinary attack order");
                }
                if (!restored && state && std::any_of(state->slots.begin(), state->slots.end(),
                        [](const auto& slot) { return slot.active; })) {
                    const auto checkpoint = world.save_checkpoint_state();
                    auto fresh = tactical::TacticalSession::from_replay(world.record(), sensors, health, movement, std::nullopt, combat, {}, table);
                    expect(checkpoint && fresh, "WAB-36: checkpoint between passes and fresh session create");
                    if (checkpoint && fresh) {
                        restored.emplace(std::move(fresh).value());
                        expect(static_cast<bool>(restored->load_checkpoint_state(checkpoint.value())),
                            "WAB-36: checkpoint between passes loads");
                        const auto recaptured = restored->save_checkpoint_state();
                        expect(recaptured && recaptured.value() == checkpoint.value(),
                            "WAB-36: checkpoint round-trip preserves canonical bytes");
                    }
                }
            }
            if (scenario == 3 || scenario == 4 || scenario == 5)
                expect(first_switch && restored, "WAB-36: ordered holder fixture exercises a switch and checkpoint continuation");
            expect(!announced, "WAB-36: a synthesised activation announces no order event");
            expect(locked.has_value() == (scenario != 1 && scenario != 5 && scenario != 7 && scenario != 8), scenario == 1
                ? "WAB-36: a human's concentrate fire stays off without autofire"
                : "WAB-36: an armed concentrate fire switches on without a command");
            const auto record = world.record();
            expect(std::none_of(record.commands.begin(), record.commands.end(), [](const auto& entry) {
                return (entry.key.sequence & (std::uint64_t{1} << 63)) != 0;
            }), "WAB-36: no recorded command carries a synthesised (bit-63) sequence");
            expect(std::none_of(record.commands.begin(), record.commands.end(), [](const auto& entry) {
                const auto* ability = std::get_if<tactical::AbilityPayload>(&entry.payload);
                return ability && ability->action == tactical::AbilityAction::activate;
            }), "WAB-36: no activation command is recorded");
            if (baseline.empty()) baseline = hashes;
            else expect(hashes == baseline, "WAB-36: concentrate autofire hashes equal at 1/2/4/8 workers");
            const auto bytes = tactical::write_replay(record);
            const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == record, "WAB-36: the concentrate autofire record round-trips");
            if (parsed) {
                auto replayed = tactical::TacticalSession::from_replay(parsed.value(), sensors, health, movement, std::nullopt, combat, {}, table);
                expect(static_cast<bool>(replayed), "WAB-36: concentrate autofire replay binds");
                if (replayed) for (const auto& hash : hashes)
                    expect(replayed.value().step(executor).value().state_sha256 == hash,
                        "WAB-36: the replay re-derives the synthesised activation");
            }
        }
    }
}

} // namespace ability_tests
