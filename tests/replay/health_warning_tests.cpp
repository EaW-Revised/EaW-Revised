#include "../support/check.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/presentation/audio/health_warning.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace tactical = eawr::sim::tactical;
using eawr::sim::math::Fixed;
eawr::testing::Checker checker;
void expect(bool condition, const char* message) { checker.check(condition, message, std::cerr, "FAIL: "); }
Fixed units(std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

struct Trace {
    std::vector<std::vector<tactical::DamageChange>> deliveries;
    std::vector<std::vector<std::uint8_t>> snapshots;
    std::vector<std::vector<std::uint8_t>> checkpoints;
    std::vector<std::string> hashes;
    std::vector<std::uint8_t> replay;
};

Trace run(std::size_t workers, bool notifications) {
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    for (std::uint32_t id = 1; id <= 32; ++id) {
        tactical::UnitState unit;
        unit.entity_id = id; unit.type_id = 40; unit.owner = id == 1 ? 1U : 2U;
        unit.position.x = units(id * 100);
        setup.units.push_back(unit);
    }
    tactical::DurabilityTable durability;
    tactical::DurabilityProfile profile;
    profile.type_id = 40; profile.max_hull = units(100);
    durability.profiles = {profile};
    auto created = tactical::TacticalSession::create(setup, {}, durability);
    expect(static_cast<bool>(created), "health notification session creates");
    if (!created) { std::cerr << created.error().message << '\n'; return {}; }
    auto session = std::move(created).value();
    session.set_damage_notifications(notifications);
    // Three ordered hits in one tick must retain every intermediate hull value.
    for (const auto [sequence, amount] : {std::pair{0U, 35}, {1U, 35}, {2U, 25}}) {
        const auto submitted = session.submit({{0, 1, sequence}, {1}, tactical::DamagePayload{units(amount), tactical::hull_target}});
        expect(static_cast<bool>(submitted), "scripted hit submits");
    }
    expect(static_cast<bool>(session.submit({{1, 1, 3}, {1}, tactical::DamagePayload{units(5), tactical::hull_target}})),
        "terminal hit submits");
    eawr::platform::ThreadWorkerAdapter executor(workers);
    eawr::platform::LiveEventLog retained({8, 1024 * 1024});
    eawr::platform::LiveEventLog tiny({8, 1});
    Trace trace;
    for (int index = 0; index < 3; ++index) {
        session.scramble_storage_for_testing();
        auto tick = session.step(executor);
        expect(static_cast<bool>(tick), "health notification tick succeeds");
        if (!tick) { std::cerr << tick.error().message << '\n'; break; }
        auto& result = tick.value();
        trace.deliveries.push_back(result.damage_changes);
        trace.snapshots.push_back(result.snapshot->canonical_bytes());
        trace.hashes.push_back(result.state_sha256);
        const auto checkpoint = session.save_checkpoint_state();
        expect(static_cast<bool>(checkpoint), "checkpoint saves with transient notifications");
        if (checkpoint) trace.checkpoints.push_back(checkpoint.value());
        retained.record(*result.snapshot, {}, {}, result.damage_changes);
        tiny.record(*result.snapshot, {}, {}, result.damage_changes);
        if (index == 0 && notifications) {
            expect(result.damage_changes.size() == 3, "all three within-tick hits are retained");
            if (result.damage_changes.size() == 3) {
                const auto& rows = result.damage_changes;
                expect(rows[0].hull_before == units(100) && rows[0].hull_after == units(65)
                    && rows[1].hull_before == units(65) && rows[1].hull_after == units(30)
                    && rows[2].hull_before == units(30) && rows[2].hull_after == units(5),
                    "per-hit receipt values precede later deliveries");
                expect(rows[0].target == tactical::DamageIdentity{1, 40, 1}
                    && !rows[0].source && rows[0].max_hull == units(100), "scripted receipt keeps identity without invented source");
            }
            const auto kept = retained.after(0, result.completed_tick);
            expect(kept.ticks.size() == 1 && kept.ticks.front().damage_changes == result.damage_changes,
                "bounded event log copies immutable delivery records");
            expect(tiny.after(0, result.completed_tick).lost_through.has_value(), "byte bound reports dropped receipt tick");
        } else expect(result.damage_changes.empty(), "terminal damage, idle ticks and disabled publication emit no receipts");
    }
    if (notifications) {
        const auto kept = retained.after(0, session.completed_tick());
        expect(!kept.ticks.empty() && kept.ticks.front().damage_changes.size() == 3,
            "earlier hits survive target destruction and later snapshots");
    }
    const auto replay = tactical::write_replay(session.record());
    expect(static_cast<bool>(replay), "replay serializes with observer enabled or disabled");
    if (replay) trace.replay = replay.value();
    return trace;
}
void projectile_source(const bool remove_source) {
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    tactical::UnitState source, target;
    source.entity_id = 1; source.type_id = 40; source.owner = 1;
    target.entity_id = 2; target.type_id = 41; target.owner = 2; target.position.x = units(100);
    setup.units = {source, target};
    tactical::DurabilityTable durability;
    tactical::DurabilityProfile source_health, target_health;
    source_health.type_id = 40; source_health.max_hull = units(100);
    target_health.type_id = 41; target_health.max_hull = units(100);
    durability.profiles = {source_health, target_health};
    tactical::DamageRules rules;
    rules.shield_recharge_frames = 30;
    rules.diminishing = {{units(0), units(1)}, {units(1), units(1)}};
    durability.damage = rules;
    tactical::WeaponProfile weapon;
    // A one-hundredth recharge truncates the initial frame window to zero.
    weapon.range = units(500); weapon.min_recharge_hundredths = 1; weapon.max_recharge_hundredths = 1;
    weapon.cone_width = units(180); weapon.cone_height = units(180);
    weapon.shot = tactical::ShotProfile{units(75), tactical::no_type_index, units(25), units(1000), true, true, {}};
    tactical::CombatProfile shooter, victim;
    shooter.type_id = 40; shooter.category_bits = 1; shooter.max_attack_distance = units(500); shooter.weapons = {weapon};
    victim.type_id = 41; victim.category_bits = 1;
    victim.collision = tactical::CollisionBox{{units(-10), units(-10), units(-10)}, {units(10), units(10), units(10)}};
    tactical::CombatTable combat;
    combat.profiles = {shooter, victim};
    const std::vector<tactical::SensorProfile> sensors{{40, units(1000)}, {41, units(1000)}};
    auto created = tactical::TacticalSession::create(setup, sensors, durability, {}, std::nullopt, combat);
    expect(static_cast<bool>(created), "projectile source fixture creates");
    if (!created) { std::cerr << created.error().message << '\n'; return; }
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "projectile source attacks");
    if (remove_source) expect(static_cast<bool>(session.submit({{1, 1, 1}, {1}, tactical::DamagePayload{units(100)}})),
        "shooter destruction after launch submits");
    const eawr::sim::InlineExecutor executor;
    bool hit = false;
    bool projectile_contact = false;
    for (int tick = 0; tick < 20; ++tick) {
        const auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "projectile source tick succeeds");
        if (!stepped) break;
        for (const auto& event : stepped.value().snapshot->combat_events())
            projectile_contact = projectile_contact || event.kind == tactical::CombatEventKind::projectile_hit;
        for (const auto& change : stepped.value().damage_changes) {
            hit = true;
            expect(change.target == tactical::DamageIdentity{2, 41, 2}, "projectile receipt preserves target identity");
            if (remove_source) {
                expect(!change.source, "HW-E01: a removed shooter cannot supply a current source type cue");
                namespace audio = eawr::presentation::audio;
                const audio::SfxEvent own, enemy;
                const audio::HealthWarningCues target_cues{&own, &own, nullptr, nullptr};
                const audio::HealthWarningCues source_cues{nullptr, nullptr, &enemy, &enemy};
                expect(audio::health_warning_cue(1.0, 0.25, 0.5, 0.1, false, change.source.has_value(), target_cues, source_cues) == nullptr,
                    "removed shooter is silent for a nonlocal surviving target despite retained projectile ownership");
                expect(audio::health_warning_cue(1.0, 0.25, 0.5, 0.1, true, change.source.has_value(), target_cues, source_cues) == &own,
                    "target-local warning still takes precedence when the shooter was removed");
            } else expect(change.source == tactical::DamageIdentity{1, 40, 1}, "projectile receipt preserves actual source type and owner");
            expect(change.hull_before == units(100) && change.hull_after == units(25), "projectile receipt records hull at delivery");
        }
    }
    expect(projectile_contact, "projectile source fixture actually lands a hit");
    expect(hit, "a surviving projectile hit publishes a damage receipt");
}

Trace environmental_source(const std::size_t workers, const bool notifications) {
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    tactical::UnitState target, field;
    target.entity_id = 1; target.type_id = 40; target.owner = 2;
    field.entity_id = 10; field.type_id = 41; field.owner = 1;
    setup.units = {target, field};
    tactical::MotionTable motion;
    motion.rules.arc_degrees = units(15); motion.rules.expansion_distance = units(300);
    motion.avoidance = tactical::AvoidanceRules{units(24), units(1), units(1), units(1), units(1),
        units(1), units(1), units(1), units(1), units(1), units(1), 1, 1, 1, 1, units(1)};
    tactical::Footprint victim, hazard;
    victim.type_id = 40; victim.layer = tactical::SpaceLayer::frigate;
    victim.asteroid_damage = true; victim.locomotor = false; victim.radius = units(3);
    hazard.type_id = 41; hazard.layer = tactical::SpaceLayer::static_object;
    hazard.asteroid_field = true; hazard.obstacle = true; hazard.radius = units(20);
    motion.footprints = {victim, hazard};
    tactical::DurabilityTable health;
    tactical::DurabilityProfile profile;
    profile.type_id = 40; profile.max_hull = units(100);
    health.profiles = {profile};
    tactical::DamageRules rules;
    rules.shield_recharge_frames = 30; rules.asteroid_damage = units(35); rules.asteroid_rate = units(1);
    health.damage = rules;
    tactical::CombatTable combat;
    tactical::CombatProfile victim_combat; victim_combat.type_id = 40;
    combat.profiles = {victim_combat};
    auto created = tactical::TacticalSession::create(setup, {}, health, motion, std::nullopt, combat);
    expect(static_cast<bool>(created), "owned environmental field fixture creates");
    if (!created) { std::cerr << created.error().message << '\n'; return {}; }
    auto session = std::move(created).value();
    session.set_damage_notifications(notifications);
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    Trace trace;
    for (unsigned tick = 0; tick < 2; ++tick) {
        session.scramble_storage_for_testing();
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "owned environmental field tick succeeds");
        if (!stepped) break;
        const auto& result = stepped.value();
        expect(result.asteroid_impacts.size() == 1, "owned field actually delivers its ordinary asteroid hit");
        trace.deliveries.push_back(result.damage_changes);
        trace.snapshots.push_back(result.snapshot->canonical_bytes());
        trace.hashes.push_back(result.state_sha256);
        const auto checkpoint = session.save_checkpoint_state();
        expect(static_cast<bool>(checkpoint), "owned field checkpoint saves");
        if (checkpoint) trace.checkpoints.push_back(checkpoint.value());
        if (!notifications) { expect(result.damage_changes.empty(), "disabled environmental publication stays empty"); continue; }
        expect(result.damage_changes.size() == 1, "one owned-field receipt per delivery");
        if (result.damage_changes.size() != 1) continue;
        const auto& change = result.damage_changes.front();
        expect(change.target == tactical::DamageIdentity{1, 40, 2}
            && change.source == tactical::DamageIdentity{10, 41, 1}, "WHZ-14: receipt retains the mod field type and local owner");
        expect(change.hull_before == units(tick == 0 ? 100 : 65) && change.hull_after == units(tick == 0 ? 65 : 30),
            "owned field records each intermediate hull value");
        namespace audio = eawr::presentation::audio;
        const audio::SfxEvent low, critical, own;
        const audio::HealthWarningCues source_cues{nullptr, nullptr, &low, &critical};
        const audio::HealthWarningCues target_cues{&own, &own, nullptr, nullptr};
        const auto before = static_cast<double>(change.hull_before.raw()) / static_cast<double>(change.max_hull.raw());
        const auto after = static_cast<double>(change.hull_after.raw()) / static_cast<double>(change.max_hull.raw());
        expect(audio::health_warning_cue(before, after, 0.75, 0.4, false, change.source && change.source->owner == 1,
            target_cues, source_cues) == (tick == 0 ? &low : &critical), "owned mod field selects its authored enemy low then critical cue");
        expect(audio::health_warning_cue(before, after, 0.75, 0.4, true, true, target_cues, source_cues) == &own,
            "target-local own warning precedes the field source");
    }
    auto replay = tactical::write_replay(session.record());
    expect(static_cast<bool>(replay), "owned field replay encodes");
    if (replay) trace.replay = std::move(replay).value();
    return trace;
}

} // namespace

int main() {
    projectile_source(false);
    projectile_source(true);
    const auto environmental_baseline = environmental_source(1, false);
    const auto environmental_expected = environmental_source(1, true);
    const auto baseline = run(1, false);
    const auto expected = run(1, true);
    expect(!expected.deliveries.empty(), "fixture actually steps");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const auto environmental = environmental_source(workers, true);
        expect(environmental.deliveries == environmental_expected.deliveries, "owned field facts agree on 1/2/4/8 workers");
        expect(environmental.hashes == environmental_baseline.hashes && environmental.snapshots == environmental_baseline.snapshots
            && environmental.checkpoints == environmental_baseline.checkpoints && environmental.replay == environmental_baseline.replay,
            "owned field publication leaves canonical state, snapshot, checkpoint and replay bytes unchanged");
        const auto actual = run(workers, true);
        expect(actual.deliveries == expected.deliveries, "delivery order and metadata identical on 1/2/4/8 workers");
        expect(actual.snapshots == baseline.snapshots && actual.hashes == baseline.hashes,
            "canonical snapshots and state hashes byte-identical with publication off");
        expect(actual.checkpoints == baseline.checkpoints && actual.replay == baseline.replay,
            "checkpoint and replay bytes identical with publication off");
    }
    return checker.failures == 0 ? 0 : 1;
}
