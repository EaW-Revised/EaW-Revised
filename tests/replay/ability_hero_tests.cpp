#include "ability_fixtures.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"
#include "../../src/sim/tactical/blast_internal.hpp"
#include "../../src/sim/tactical/checkpoint_fields_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ability_tests {


// FS-23/FC-51/FD-32: one team request approaches and retries independently of the
// ordinary target scan. Application starts recharge; UFS-21 also resolves ended approaches.
void test_buzz_team_request() {
    constexpr tactical::TypeId team_type = 40, craft_type = 41, droid_type = 42;
    std::vector<std::string> reference;
    std::map<std::string, std::vector<std::string>> case_references;
    const auto compare = [&](const std::string& name, const std::vector<std::string>& hashes) {
        const auto [entry, inserted] = case_references.emplace(name, hashes);
        if (!inserted) expect(entry->second == hashes, "UFS-21: regression hashes equal on 1/2/4/8 workers");
    };
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = setup();
        initial.units = {{10, team_type, 1, at(0, 0), math::identity_quat(), {}},
            {11, craft_type, 1, at(0, 0), math::identity_quat(), {}},
            {20, team_type, 1, at(0, 2000), math::identity_quat(), {}},
            {21, craft_type, 1, at(0, 2000), math::identity_quat(), {}},
            {30, plain_type, 2, at(4000, 4000), math::identity_quat(), {}}};
        initial.squadrons = {{10, {11}}, {20, {21}}};
        auto health = durability();
        for (const auto type : {team_type, craft_type, droid_type}) {
            tactical::DurabilityProfile profile;
            profile.type_id = type; profile.max_hull = units(20);
            health.profiles.push_back(profile);
        }
        auto movement = motion();
        tactical::CraftProfile craft;
        craft.type_id = craft_type;
        craft.max_speed = units(3); craft.min_speed = units(2);
        craft.rate_of_turn = units(6); craft.lift = units(6);
        craft.thrust = units(1); craft.roll_rate = units(3); craft.bank_angle = units(70);
        craft.strafe_distance = units(500);
        movement.squadrons.craft = {craft};
        movement.squadrons.squadrons = {{team_type, {craft_type}, {at(0, 0)},
            units(1000), units(200), units(300), units(25)}};
        tactical::AbilityProfile buzz;
        buzz.kind = tactical::AbilityKind::buzz_droids; buzz.recharge_frames = 60;
        buzz.buzz = tactical::BuzzDroidsProfile{droid_type, units(5), units(400), units(300), units(50),
            units(5), units(1), units(20), 1000, 2, 0};
        tactical::AbilityTable table; table.profiles = {unit_of(team_type, buzz)};
        auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "FS-23: team buzz request fixture creates");
        if (!created) continue;
        auto world = std::move(created).value();
        expect(world.squadron_state(10).has_value() && world.squadron_state(20).has_value(),
            "FS-23: each ability holder has its authored team membership");
        tactical::AbilityPayload input{tactical::AbilityKind::buzz_droids, tactical::AbilityAction::activate};
        input.position = at(500, 0);
        auto far = input; far.position = at(5000, 2000);
        expect(static_cast<bool>(world.submit(command(0, 1, 0, 10, input))), "FS-23: one team point request submits");
        expect(static_cast<bool>(world.submit(command(0, 1, 1, 20, far))), "FS-23: distant request submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::optional<std::uint64_t> applied;
        for (std::uint64_t frame = 0; frame < 120; ++frame) {
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "FC-51: retained team request step succeeds");
            if (!step) { std::cerr << step.error().message << '\n'; break; }
            hashes.push_back(step.value().state_sha256);
            const auto holder = world.ability_state(10);
            const auto blocked = world.ability_state(20);
            expect(blocked && blocked->pending_buzz_position == far.position && blocked->slots[0].ready_tick == 0,
                "FC-51: continuing range refusal retains the marker and never starts recharge");
            const auto units_now = world.units();
            const auto count = std::count_if(units_now.begin(), units_now.end(),
                [](const tactical::UnitState& unit) { return unit.type_id == droid_type; });
            if (count != 0 && !applied) applied = frame;
            if (!applied) {
                expect(holder && holder->pending_buzz_position == input.position && holder->slots[0].ready_tick == 0,
                    "FC-51: out-of-range refusal keeps the request without recharge");
            } else {
                expect(count == 1 && holder && !holder->pending_buzz_position
                    && holder->slots[0].started_tick == *applied && holder->slots[0].ready_tick == *applied + 60,
                    "FS-08/23: successful retry creates exactly one droid and clears only that request");
                const auto droid = std::find_if(units_now.begin(), units_now.end(),
                    [](const tactical::UnitState& unit) { return unit.type_id == droid_type; });
                expect(droid != units_now.end() && droid->owner == 1 && droid->position == *input.position,
                    "WAB-71: the retry's droid appears at its retained point for the holder's owner");
            }
            if (frame == 0) expect(!applied && std::count_if(step.value().snapshot->events().begin(),
                step.value().snapshot->events().end(), [](const tactical::Event& event) {
                    return event.kind == tactical::EventKind::order_accepted && event.order == tactical::OrderKind::ability;
                }) == 2, "FS-23: request admission precedes application, including out-of-range points");
        }
        expect(applied && *applied > 1 && *applied < 120,
            "FD-32: the admitted approach enters range and applies without another command");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "FC-51: retained request and ordered allocation agree on 1/2/4/8 workers");
        const auto record = world.record();
        const auto encoded = tactical::write_replay(record);
        const auto parsed = encoded ? tactical::parse_replay(encoded.value())
            : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(static_cast<bool>(parsed), "FS-23: team request replay round-trips");
        if (!parsed) continue;
        auto replayed = tactical::TacticalSession::from_replay(parsed.value(), {}, health, movement,
            std::nullopt, {}, {}, table);
        expect(static_cast<bool>(replayed), "FS-23: retained request replay creates");
        if (replayed) {
            std::vector<std::string> replay_hashes;
            while (replayed.value().completed_tick() < record.final_tick_count) {
                const auto step = replayed.value().step(executor);
                if (!step) break;
                replay_hashes.push_back(step.value().state_sha256);
            }
            expect(replay_hashes == hashes, "FC-51: replay reconstructs retained points and delayed application");
        }
        // UFS-21: preserve BASE admission for points that cannot be reached by approach.
        for (const auto distance : {3, 5}) {
            auto refused = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
            expect(static_cast<bool>(refused), "WAB-70: minimum-range team fixture creates");
            if (!refused) continue;
            auto& near_world = refused.value();
            auto near = input; near.position = at(distance, 0);
            expect(static_cast<bool>(near_world.submit(command(0, 1, 0, 10, near))),
                "WAB-70: near-point command submits for admission checking");
            std::vector<std::string> near_hashes;
            for (std::uint64_t frame = 0; frame < 120; ++frame) {
                const auto step = near_world.step(executor);
                expect(static_cast<bool>(step), "WAB-70: minimum-range refusal steps");
                if (!step) break;
                near_hashes.push_back(step.value().state_sha256);
                if (frame == 0) expect(std::any_of(step.value().snapshot->events().begin(),
                    step.value().snapshot->events().end(), [](const tactical::Event& event) {
                        return event.kind == tactical::EventKind::order_rejected
                            && event.reason == tactical::RejectReason::invalid_position;
                    }), "WAB-70: a point at or inside the minimum is refused at admission");
                const auto state = near_world.ability_state(10);
                expect(state && !state->pending_buzz_position && state->slots[0].ready_tick == 0,
                    "WAB-70: refused near point leaves no retained request or recharge");
                const auto live = near_world.units();
                expect(std::none_of(live.begin(), live.end(), [](const tactical::UnitState& unit) {
                    return unit.type_id == droid_type;
                }), "WAB-70: refused near point never creates a swarm");
            }
            compare("refused-" + std::to_string(distance), near_hashes);
        }
        for (const auto distance : {6, 7}) for (const bool ordinary_attack : {false, true}) {
            auto boundary = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
            expect(static_cast<bool>(boundary), "UFS-21: near-band team fixture creates");
            if (!boundary) continue;
            auto& boundary_world = boundary.value();
            if (ordinary_attack) expect(static_cast<bool>(boundary_world.submit(
                command(0, 1, 0, 10, tactical::AttackPayload{30, tactical::attack_hull}))),
                "UFS-21: near-band request also covers a team already attacking");
            auto near = input; near.position = at(distance, 0);
            expect(static_cast<bool>(boundary_world.submit(command(0, 1, 1, 10, near))),
                "UFS-21: point just above the minimum submits");
            std::vector<std::string> boundary_hashes;
            for (std::uint64_t frame = 0; frame < 120; ++frame) {
                const auto step = boundary_world.step(executor);
                expect(static_cast<bool>(step), "UFS-21: near-band request steps");
                if (!step) break;
                boundary_hashes.push_back(step.value().state_sha256);
            }
            const auto state = boundary_world.ability_state(10);
            const auto live = boundary_world.units();
            const auto count = std::count_if(live.begin(), live.end(), [](const tactical::UnitState& unit) {
                return unit.type_id == droid_type;
            });
            expect(state && !state->pending_buzz_position && count == 1 && state->slots[0].ready_tick > 0,
                "UFS-21: the 6/7-unit approach applies exactly once and starts recharge");
            compare("boundary-" + std::to_string(distance) + (ordinary_attack ? "-attack" : "-idle"), boundary_hashes);
        }
        // UFS-21: Stop explicitly cancels an unsuccessful retained approach.
        auto stopped = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
        expect(static_cast<bool>(stopped), "UFS-21: completed approach fixture creates");
        if (stopped) {
            auto& stopped_world = stopped.value();
            expect(static_cast<bool>(stopped_world.submit(command(0, 1, 0, 10, far))),
                "UFS-21: unsuccessful approach submits");
            expect(static_cast<bool>(stopped_world.submit(command(1, 1, 1, 10, tactical::StopPayload{}))),
                "UFS-21: approach stops at rest before entering range");
            std::vector<std::string> stopped_hashes;
            for (std::uint64_t frame = 0; frame < 120; ++frame) {
                const auto step = stopped_world.step(executor);
                expect(static_cast<bool>(step), "UFS-21: completed approach steps");
                if (!step) break;
                stopped_hashes.push_back(step.value().state_sha256);
                const auto state = stopped_world.ability_state(10);
                expect(state && state->slots[0].ready_tick == 0
                    && state->pending_buzz_position.has_value() == (frame < 1),
                    "UFS-21: Stop cancels the admitted approach without recharge");
                const auto live = stopped_world.units();
                expect(std::none_of(live.begin(), live.end(), [](const tactical::UnitState& unit) {
                    return unit.type_id == droid_type;
                }), "UFS-21: completed unsuccessful approach creates no swarm");
            }
            compare("stop", stopped_hashes);
        }
        // UFS-21: all accepted replacements end the approach, independent of completion.
        const std::vector<tactical::CommandPayload> replacements{
            tactical::AttackPayload{30, tactical::attack_hull},
            tactical::GuardPayload{at(0, 0), 20},
            tactical::AttackMovePayload{at(0, 0), 20},
            tactical::MovePayload{*far.position}};
        for (std::size_t replacement = 0; replacement < replacements.size(); ++replacement) {
            auto interrupted = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
            expect(static_cast<bool>(interrupted), "UFS-21: replacement fixture creates");
            if (!interrupted) continue;
            auto& interrupted_world = interrupted.value();
            expect(static_cast<bool>(interrupted_world.submit(command(0, 1, 0, 10, far))),
                "UFS-21: pending approach submits before replacement");
            expect(static_cast<bool>(interrupted_world.submit(command(1, 1, 1, 10, replacements[replacement]))),
                "UFS-21: ordinary replacement submits");
            std::vector<std::string> interrupted_hashes;
            for (unsigned frame = 0; frame < 300; ++frame) {
                const auto step = interrupted_world.step(executor);
                expect(static_cast<bool>(step), "UFS-21: interrupted approach steps");
                if (!step) break;
                interrupted_hashes.push_back(step.value().state_sha256);
                const auto state = interrupted_world.ability_state(10);
                expect(state && state->pending_buzz_position.has_value() == (frame == 0)
                    && state->slots[0].ready_tick == 0,
                    "UFS-21: replacement clears the old request without swarm or recharge");
                const auto live = interrupted_world.units();
                expect(std::none_of(live.begin(), live.end(), [](const tactical::UnitState& unit) {
                    return unit.type_id == droid_type;
                }), "UFS-21: interrupted request never fires at its old point");
            }
            compare("replacement-" + std::to_string(replacement), interrupted_hashes);
        }
        // UFS-21: the narrow activation band keeps the 7-unit interrupt unready.
        auto narrow_table = table;
        narrow_table.profiles[0].abilities[0].buzz->max_range = units(6);
        auto near_interrupt = tactical::TacticalSession::create(initial, {}, health, movement,
            std::nullopt, {}, {}, narrow_table);
        expect(static_cast<bool>(near_interrupt), "UFS-21: near-band interrupt fixture creates");
        if (near_interrupt) {
            auto& interrupted_world = near_interrupt.value();
            auto near = input; near.position = at(7, 0);
            expect(static_cast<bool>(interrupted_world.submit(command(0, 1, 0, 10, near))),
                "UFS-21: near-band approach submits");
            expect(static_cast<bool>(interrupted_world.submit(command(1, 1, 1, 10,
                tactical::AttackPayload{30, tactical::attack_hull}))), "UFS-21: player Attack interrupts near-band approach");
            std::vector<std::string> near_interrupt_hashes;
            for (unsigned frame = 0; frame < 300; ++frame) {
                const auto step = interrupted_world.step(executor);
                expect(static_cast<bool>(step), "UFS-21: near-band interrupt steps");
                if (!step) break;
                near_interrupt_hashes.push_back(step.value().state_sha256);
                const auto state = interrupted_world.ability_state(10);
                expect(state && state->pending_buzz_position.has_value() == (frame == 0)
                    && state->slots[0].ready_tick == 0,
                    "UFS-21: unready near-band Attack resolves the request without recharge");
                const auto live = interrupted_world.units();
                expect(std::none_of(live.begin(), live.end(), [](const auto& unit) { return unit.type_id == droid_type; }),
                    "UFS-21: the interrupted near-band marker never spawns droids");
            }
            compare("near-band-interrupt", near_interrupt_hashes);
        }
        // UFS-21: losing the base target cancels the failed special approach.
        auto lost = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
        expect(static_cast<bool>(lost), "UFS-21: target-loss fixture creates");
        if (lost) {
            auto& lost_world = lost.value();
            expect(static_cast<bool>(lost_world.submit(command(0, 1, 0, 10,
                tactical::AttackPayload{30, tactical::attack_hull}))), "UFS-21: ordinary base attack submits");
            expect(static_cast<bool>(lost_world.submit(command(0, 1, 1, 10, far))), "UFS-21: retained approach submits");
            std::vector<std::string> lost_hashes;
            for (unsigned frame = 0; frame < 120; ++frame) {
                if (frame == 1) expect(static_cast<bool>(lost_world.stage_remove(30)), "UFS-21: approach base target leaves");
                const auto step = lost_world.step(executor);
                expect(static_cast<bool>(step), "UFS-21: target-loss approach steps");
                if (!step) break;
                lost_hashes.push_back(step.value().state_sha256);
                const auto state = lost_world.ability_state(10);
                expect(state && state->pending_buzz_position.has_value() == (frame == 0)
                    && state->slots[0].ready_tick == 0,
                    "UFS-21: lost base drops its failed request without recharge");
                const auto live = lost_world.units();
                expect(std::none_of(live.begin(), live.end(), [](const auto& unit) { return unit.type_id == droid_type; }),
                    "UFS-21: lost approach never creates a swarm");
            }
            compare("lost-base", lost_hashes);
        }
        // FS-23/WMV-18 (#3491): base loss converts the object base to a position;
        // autonomous targeting can acquire another live target in the preserved formation.
        auto reacquire_initial = initial;
        reacquire_initial.units.push_back({31, plain_type, 2, at(100, 0), math::identity_quat(), {}});
        tactical::CombatTable reacquire_combat;
        tactical::CombatProfile enemy; enemy.type_id = plain_type; enemy.category_bits = 1;
        tactical::CombatProfile fighter; fighter.type_id = craft_type; fighter.category_bits = 1;
        fighter.max_attack_distance = units(2000);
        tactical::WeaponProfile weapon; weapon.range = units(2000);
        weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
        fighter.weapons = {weapon};
        reacquire_combat.profiles = {enemy, fighter};
        const std::vector<tactical::SensorProfile> reacquire_sensors{{craft_type, units(10000)}};
        auto reacquired = tactical::TacticalSession::create(reacquire_initial, reacquire_sensors, health, movement,
            std::nullopt, reacquire_combat, {}, table);
        expect(static_cast<bool>(reacquired), "FS-23: base-loss reacquisition fixture creates");
        if (reacquired) {
            auto& reacquire_world = reacquired.value();
            expect(static_cast<bool>(reacquire_world.submit(command(0, 1, 0, 10,
                tactical::AttackPayload{30, tactical::attack_hull}))), "FS-23: removable base target submits");
            std::vector<std::string> reacquire_hashes;
            const auto first = reacquire_world.step(executor);
            expect(static_cast<bool>(first), "FS-23: base attack steps");
            if (first) reacquire_hashes.push_back(first.value().state_sha256);
            expect(static_cast<bool>(reacquire_world.stage_remove(30)), "FS-23: ordinary base target leaves");
            bool found_preserved = false;
            for (unsigned frame = 0; frame < 300; ++frame) {
                const auto step = reacquire_world.step(executor);
                expect(static_cast<bool>(step), "FS-23: reacquisition steps");
                if (!step) break;
                reacquire_hashes.push_back(step.value().state_sha256);
                const auto team = reacquire_world.squadron_state(10);
                if (frame == 0) expect(team && team->formation && team->formation->base_target == 0,
                    "WMV-18: base loss preserves the formation with a position base");
                // FS-23 checks the team's ordinary target; movement destination
                // admission separately retains WMV-17's base allowance.
                if (team && team->formation && team->target == 31 && team->formation->base_target == 0) {
                    found_preserved = true;
                    break;
                }
            }
            expect(found_preserved, "FS-23: live ordinary target is acquired in the preserved formation");
            compare("preserved-formation", reacquire_hashes);
            if (!found_preserved) continue;

            // WMV-18: an actually absent formation is distinct from base loss. Stage
            // that state through the checkpoint fields, as in the fighter scan contracts,
            // preserving the acquired target, live units and sealed command log.
            namespace checkpoint = tactical::checkpoint_detail;
            const auto saved = reacquire_world.save_checkpoint_state();
            expect(static_cast<bool>(saved), "FS-23: absent formation staging checkpoint saves");
            if (!saved) continue;
            checkpoint::Access::State staged(reacquire_initial, reacquire_sensors, health, movement,
                std::nullopt, reacquire_combat, {}, table, {}, {});
            checkpoint::Reader reader(saved.value());
            std::uint32_t version{};
            reader(version);
            checkpoint::Access::state(reader, staged);
            std::vector<tactical::session_detail::LiveUnit> live;
            reader(live);
            const auto tail = reader.remaining();
            staged.minds.at(10).formation.reset();
            checkpoint::Writer writer;
            writer(version);
            checkpoint::Access::state(writer, std::as_const(staged));
            writer(live);
            writer.bytes.insert(writer.bytes.end(), saved.value().end() - static_cast<std::ptrdiff_t>(tail), saved.value().end());
            auto absent = tactical::TacticalSession::from_replay(reacquire_world.record_through_next_tick(),
                reacquire_sensors, health, movement, std::nullopt, reacquire_combat, {}, table);
            expect(static_cast<bool>(absent), "FS-23: absent formation preserves the sealed command log");
            if (!absent) continue;
            auto& absent_world = absent.value();
            const auto loaded = absent_world.load_checkpoint_state(writer.bytes);
            expect(static_cast<bool>(loaded), "FS-23: absent formation staged state restores");
            if (!loaded) continue;
            const auto before_point = absent_world.squadron_state(10);
            expect(before_point && !before_point->formation && before_point->target == 31
                && before_point->target_hardpoint == tactical::attack_hull,
                "FS-23: special point starts with a live ordinary target and genuinely absent formation");
            auto point = input;
            const auto holder = absent_world.units();
            const auto container = std::find_if(holder.begin(), holder.end(), [](const auto& unit) { return unit.entity_id == 10; });
            if (container != holder.end()) point.position = math::Vec3{math::Fixed::from_raw(container->position.x.raw() + units(100).raw()),
                container->position.y, container->position.z};
            expect(static_cast<bool>(absent_world.submit(command(absent_world.completed_tick(), 1, 1, 10, point))),
                "FS-23: absent formation special point submits");
            const auto admitted = absent_world.step(executor);
            expect(static_cast<bool>(admitted), "FS-23: absent formation admission steps");
            std::vector<std::string> absent_hashes;
            if (admitted) {
                absent_hashes.push_back(admitted.value().state_sha256);
                expect(admitted.value().diagnostics.empty(), "FS-23: absent formation special point is admitted");
            }
            const auto team = absent_world.squadron_state(10);
            expect(team && team->target == 31 && team->target_hardpoint == tactical::attack_hull
                && team->formation && team->formation->base_target == 0
                && team->formation->base_position == *point.position
                && team->formation->attack_target == team->target
                && team->formation->attack_hardpoint == team->target_hardpoint,
                "FS-23: absent formation retains its live ordinary target in the new formation");
            const auto queued = absent_world.ability_state(10);
            expect(queued && queued->pending_buzz_position == point.position && queued->slots[0].ready_tick == 0,
                "FS-23: absent formation queues the special point without recharge");
            compare("absent-formation", absent_hashes);
        }
        auto fighting = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
        expect(static_cast<bool>(fighting), "FS-23: ordinary-target team fixture creates");
        if (!fighting) continue;
        auto& fighting_world = fighting.value();
        expect(static_cast<bool>(fighting_world.submit(command(0, 1, 0, 10, tactical::AttackPayload{30, tactical::attack_hull}))),
            "FS-23: ordinary team attack submits before special point");
        auto in_range = input; in_range.position = at(100, 0);
        expect(static_cast<bool>(fighting_world.submit(command(0, 1, 1, 10, in_range))),
            "FSC-33: in-range team point submits");
        const auto admitted = fighting_world.step(executor);
        expect(static_cast<bool>(admitted), "FS-23: ordinary target and special point admission steps");
        const auto team = fighting_world.squadron_state(10);
        expect(team && team->target == 30 && team->target_hardpoint == tactical::attack_hull && team->formation
            && team->formation->attack_target == 30 && team->formation->base_target == 30
            && team->formation->base_position == *in_range.position,
            "FS-23: special approach retains the ordinary target in both team and movement formation");
        const auto queued = fighting_world.ability_state(10);
        expect(queued && queued->pending_buzz_position == in_range.position && queued->slots[0].ready_tick == 0,
            "FSC-33: even an in-range team point is queued at t without application");
        std::vector<std::string> fighting_hashes;
        if (admitted) fighting_hashes.push_back(admitted.value().state_sha256);
        const auto applied_next = fighting_world.step(executor);
        if (applied_next) fighting_hashes.push_back(applied_next.value().state_sha256);
        compare("prior-formation", fighting_hashes);
        expect(static_cast<bool>(applied_next), "FSC-33: in-range team retry steps at t+1");
        const auto applied_state = fighting_world.ability_state(10);
        const auto fighting_units = fighting_world.units();
        expect(applied_state && !applied_state->pending_buzz_position && applied_state->slots[0].started_tick == 1
            && applied_state->slots[0].ready_tick == 61
            && std::count_if(fighting_units.begin(), fighting_units.end(), [](const tactical::UnitState& unit) {
                return unit.type_id == droid_type;
            }) == 1, "FSC-33: the t+1 retry applies once and begins the full recharge");
    }
}


// WAB-70..74: a BUZZ_DROIDS activation at a point creates the droid object there; from its arming
// frame it wears itself down, chases the nearest enemy mover in the chase radius around the creation
// point and damages every one in damage reach. 1/2/4/8 workers give the same hashes.
void test_buzz_droids() {
    using K = tactical::AbilityKind;
    constexpr tactical::TypeId droid_type = 30;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = setup();
        initial.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}},
            {2, frigate_type, 2, at(160, 0), math::identity_quat(), {}},
            {3, corvette_type, 2, at(100, 1000), math::identity_quat(), {}},
            {4, plain_type, 1, at(0, 2000), math::identity_quat(), {}}};
        auto health = durability();
        for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
        tactical::DurabilityProfile droid_health;
        droid_health.type_id = droid_type; droid_health.max_hull = units(20);
        health.profiles.push_back(droid_health);
        tactical::AbilityProfile buzz;
        buzz.kind = K::buzz_droids; buzz.recharge_frames = 60;
        buzz.buzz = tactical::BuzzDroidsProfile{droid_type, units(5), units(400), units(300), units(50),
            units(5), units(1), units(20), 3, 2, 0};
        tactical::AbilityTable table; table.profiles = {unit_of(plain_type, buzz)};
        tactical::CombatTable combat;
        for (const auto type : {corvette_type, frigate_type, plain_type}) {
            tactical::CombatProfile profile; profile.type_id = type; profile.category_bits = 2;
            profile.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
            combat.profiles.push_back(profile);
        }
        auto created = tactical::TacticalSession::create(initial,
            std::vector<tactical::SensorProfile>{{corvette_type, units(1000)}, {frigate_type, units(1000)}, {plain_type, units(1000)}},
            health, motion(), std::nullopt, combat, {}, table);
        expect(static_cast<bool>(created), "WAB-70: buzz-droid fixture creates");
        if (!created) continue;
        auto world = std::move(created).value();
        tactical::AbilityPayload input{K::buzz_droids, tactical::AbilityAction::activate};
        input.position = at(100, 0);
        auto far = input;
        far.position = at(0, 2500);
        auto near = input;
        near.position = at(0, 2003);
        expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, input))), "WAB-70: buzz-droid point input accepted");
        expect(static_cast<bool>(world.submit(command(0, 1, 1, 4, far))), "WAB-70: out-of-reach point queues for rejection");
        expect(static_cast<bool>(world.submit(command(0, 1, 2, 4, near))), "WAB-70: too-near point queues for rejection");
        expect(static_cast<bool>(world.submit(command(1, 1, 3, 1, input))), "WAB-71: recharging input queues for rejection");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::optional<eawr::sim::EntityId> droid;
        bool droid_died = false;
        for (unsigned frame = 0; frame < 26; ++frame) {
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "WAB-72: buzz-droid step succeeds");
            if (!step) { std::cerr << step.error().message << '\n'; break; }
            hashes.push_back(step.value().state_sha256);
            const auto& events = step.value().snapshot->events();
            if (frame == 0) {
                const auto units_now = world.units();
                const auto found = std::find_if(units_now.begin(), units_now.end(),
                    [&](const tactical::UnitState& unit) { return unit.type_id == droid_type; });
                expect(found != units_now.end() && found->owner == 1 && found->position == at(100, 0)
                    && std::count_if(units_now.begin(), units_now.end(),
                        [&](const tactical::UnitState& unit) { return unit.type_id == droid_type; }) == 1,
                    "WAB-71: one droid object at the point for the activating owner");
                if (found != units_now.end()) droid = found->entity_id;
                expect(world.ability_state(1) && !world.ability_state(1)->slots[0].active
                    && world.ability_state(1)->slots[0].ready_tick == 60, "WAB-71: instant activation starts the full recharge");
                expect(std::count_if(events.begin(), events.end(), [](const tactical::Event& event) {
                    return event.kind == tactical::EventKind::order_rejected && event.unit == 4
                        && event.reason == tactical::RejectReason::invalid_position; }) == 2,
                    "WAB-70: points outside the authored planar reach, or inside the minimum, are refused");
            }
            if (frame == 1) expect(std::any_of(events.begin(), events.end(), [](const tactical::Event& event) {
                return event.kind == tactical::EventKind::order_rejected && event.unit == 1; }), "WAB-71: recharge refuses another activation");
            if (!droid) continue;
            const auto droid_now = world.durability_state(*droid);
            const auto units_now = world.units();
            const auto unit = std::find_if(units_now.begin(), units_now.end(),
                [&](const tactical::UnitState& entry) { return entry.entity_id == *droid; });
            if (frame < 3) expect(droid_now && droid_now->hull == units(20) && unit != units_now.end()
                && unit->position == at(100, 0), "WAB-72: the droids wait for their arming frame");
            if (frame == 3) expect(droid_now && droid_now->hull == units(19) && unit != units_now.end()
                && unit->position == at(120, 0) && world.durability_state(2)->hull == units(3600),
                "WAB-74: armed droids out of damage reach close on the nearest enemy at full speed");
            if (frame == 4) expect(droid_now && droid_now->hull == units(18) && unit != units_now.end()
                && unit->position == at(120, 0) && world.durability_state(2)->hull == units(3595)
                && world.durability_state(3)->hull == units(3600),
                "WAB-72/73: droids in reach stay and damage every enemy there; one outside the chase radius is untouched");
            if (!droid_now && !droid_died) {
                droid_died = true;
                expect(frame == 22 && std::any_of(events.begin(), events.end(), [&](const tactical::Event& event) {
                    return event.kind == tactical::EventKind::unit_destroyed && event.unit == *droid; }),
                    "WAB-72: the droids' own wear destroys them after their authored health");
            }
        }
        expect(droid_died, "WAB-72: the droid object dies of its own wear");
        const auto record = world.record();
        const auto encoded = tactical::write_replay(record);
        const auto parsed = encoded ? tactical::parse_replay(encoded.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(parsed && parsed.value() == record, "WAB-70: buzz-droid point command round-trips");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "WAB-72: buzz-droid hashes equal on 1/2/4/8 workers");
    }
}


void test_hostile_buzz_droids() {
    using K = tactical::AbilityKind;
    constexpr tactical::TypeId droid_type = 30;
    constexpr tactical::TypeId victim_type = 31;
    for (const bool droid_locomotor : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, plain_type, 2, at(300, 0), math::identity_quat(), {}},
                {3, victim_type, 2, at(100, 0), math::identity_quat(), {}}};
            auto health = durability();
            tactical::DurabilityProfile droid_health;
            droid_health.type_id = droid_type; droid_health.max_hull = units(20);
            health.profiles.push_back(droid_health);
            auto victim_health = droid_health;
            victim_health.type_id = victim_type; victim_health.max_hull = units(100);
            health.profiles.push_back(victim_health);
            tactical::AbilityProfile buzz;
            buzz.kind = K::buzz_droids; buzz.recharge_frames = 60;
            buzz.buzz = tactical::BuzzDroidsProfile{droid_type, units(5), units(400), units(300), units(50),
                units(20), units(1), units(20), 1, 2, 0};
            tactical::AbilityTable table; table.profiles = {unit_of(plain_type, buzz)};
            tactical::CombatTable combat;
            for (const auto type : {droid_type, victim_type}) {
                tactical::CombatProfile profile; profile.type_id = type; profile.category_bits = 2;
                combat.profiles.push_back(profile);
            }
            auto movement = motion();
            auto droid_motion = movement.profiles.front();
            droid_motion.type_id = droid_type;
            movement.profiles.push_back(droid_motion);
            tactical::Footprint droid_footprint;
            droid_footprint.type_id = droid_type; droid_footprint.locomotor = droid_locomotor;
            auto victim_footprint = droid_footprint;
            victim_footprint.type_id = victim_type; victim_footprint.locomotor = true;
            movement.footprints = {droid_footprint, victim_footprint};
            auto created = tactical::TacticalSession::create(initial, {}, health, movement,
                std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "WAB-73: hostile droids with authored locomotor flags create");
            if (!created) continue;
            auto world = std::move(created).value();
            tactical::AbilityPayload launch{K::buzz_droids, tactical::AbilityAction::activate};
            launch.position = at(100, 0);
            expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, launch))), "first hostile swarm launches");
            launch.position = at(140, 0);
            expect(static_cast<bool>(world.submit(command(0, 2, 0, 2, launch))), "second hostile swarm launches");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            std::optional<eawr::sim::EntityId> first, second;
            for (unsigned frame = 0; frame < 3; ++frame) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "WAB-72: hostile swarm commit succeeds after an earlier droid kill");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
                if (frame == 0) {
                    for (const auto& unit : world.units()) {
                        if (unit.type_id != droid_type) continue;
                        (unit.owner == 1 ? first : second) = unit.entity_id;
                    }
                    expect(first && second && *first < *second, "hostile swarm fixture orders the killing droid first");
                }
                if (frame == 1 && first && second) {
                    const auto survivor = world.durability_state(*first);
                    const auto victim = world.durability_state(*second);
                    expect(survivor && survivor->hull == units(19), "killed hostile droid does not commit its staged damage");
                    expect(droid_locomotor ? !victim : victim && victim->hull == units(19),
                        "WAB-73: motion data alone does not make a droid a locomotor victim");
                    const auto& events = step.value().snapshot->events();
                    expect(std::count_if(events.begin(), events.end(), [&](const tactical::Event& event) {
                        return event.kind == tactical::EventKind::unit_destroyed && event.unit == *second;
                    }) == (droid_locomotor ? 1 : 0), "hostile droid destruction is emitted exactly once");
                    const auto stationary = world.durability_state(3);
                    expect(stationary && stationary->hull == units(80),
                        "WAB-73: a locomotor victim without a motion profile still takes damage");
                }
            }
            expect(hashes.size() == 3, "hostile swarm service continues after the killing tick");
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "hostile swarm hashes equal on 1/2/4/8 workers");
        }
    }
}


void test_banked_ability_geometry() {
    using K = tactical::AbilityKind;
    // ABF-01/02: distinguish the rolled bone/box from the level geometry, including
    // collection admission outside the level box. No retail fixture or pin is changed.
    for (const unsigned scenario : {0U, 1U, 2U, 3U, 4U}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const auto kind = scenario == 0 ? K::energy_weapon : scenario == 1 ? K::buzz_droids : K::corrupt_systems;
            auto initial = setup();
            const tactical::PlayerId victim_owner = scenario == 4 ? 1U : 2U;
            initial.units = {{2, frigate_type, victim_owner, at(0, 0), math::identity_quat(), {}}};
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
            health.profiles[1].hardpoints = {{tactical::HardpointRole::other, true, units(100)},
                {tactical::HardpointRole::other, true, units(100)}};
            health.profiles[1].destroyed_with_hardpoints = true;
            constexpr tactical::TypeId droid_type = 30;
            tactical::DurabilityProfile droid_health;
            droid_health.type_id = droid_type; droid_health.max_hull = units(20);
            health.profiles.push_back(droid_health);
            auto movement = motion();
            movement.profiles[1].rate_of_turn = decimal("0.5");
            movement.profiles[1].turn_in_place_slowdown = units(1);
            movement.profiles[1].roll_rate = units(3);
            movement.profiles[1].bank_angle = units(60);
            tactical::CombatProfile target;
            target.type_id = frigate_type; target.category_bits = 2;
            target.hardpoints = {{0, at(0, 100, 0), true}, {1, at(0, 0, 100), true}};
            target.collision = tactical::CollisionBox{at(-1, -20, -100), at(1, 20, 100)};
            tactical::CombatProfile source; source.type_id = plain_type; source.category_bits = 1;
            tactical::CombatTable combat; combat.profiles = {target, source};
            tactical::AbilityProfile ability;
            ability.kind = kind; ability.recharge_frames = 100;
            ability.expiration_frames = scenario == 0 ? 20 : 0;
            ability.recharge_at_activation = scenario == 2 || scenario == 3;
            ability.effective_radius = units(5); ability.gui_activated_ability_name = "banked";
            auto holder = unit_of(plain_type, ability);
            tactical::SpecialAbilityProfile nested;
            nested.name = "banked"; nested.style = tactical::SpecialActivationStyle::user_input;
            nested.kind = scenario == 0 ? tactical::SpecialAbilityKind::energy_weapon : tactical::SpecialAbilityKind::corrupt_systems;
            nested.service_interval = 1; nested.beam_max_range = units(500);
            nested.damage_per_frame = units(5); nested.filter.applicable_categories = 2;
            nested.corrupt_max_range = units(350); nested.corrupt_duration_frames = 30;
            nested.corrupt_drain_per_frame = units(1); nested.corrupt_inaccuracy = units(10);
            if (scenario == 1) holder.abilities[0].buzz = tactical::BuzzDroidsProfile{droid_type,
                units(1), units(400), units(300), units(10), units(5), units(1), units(20), 1, 2, 0};
            else if (scenario == 4) {
                holder.abilities.clear();
                nested.kind = tactical::SpecialAbilityKind::force_healing;
                nested.style = tactical::SpecialActivationStyle::space_automatic;
                nested.initially_enabled = true; nested.heal_range = units(5);
                nested.heal_amount = units(5); nested.heal_interval_frames = 1;
                holder.special = {nested};
            } else holder.special = {nested};
            tactical::AbilityTable table; table.profiles = {holder}; table.beam_damage_type = 0;
            auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "ABF-01: banked ability fixture creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto& world = created.value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(static_cast<bool>(world.submit(command(0, victim_owner, 0, 2, tactical::FacePayload{at(0, -10000)}))),
                "ABF-01: victim starts an in-place banked turn");
            if (scenario == 4) expect(static_cast<bool>(world.submit(command(49, 1, 1, 2, tactical::DamagePayload{units(50), 0}))),
                "ABF-02: banked healing victim has a damaged standing point");
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 50; ++frame) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "ABF-01: bank warmup succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256 + step.value().snapshot->sha256());
            }
            const auto drawn = world.snapshot()->instances().front().fixed_transform;
            const auto state = world.units().front();
            const auto level = math::to_matrix(state.rotation, state.position).value();
            expect(drawn != level, "ABF-01: fixture has nonzero bank");
            auto point = math::transform_point(scenario == 3 ? level : drawn,
                scenario < 2 ? at(0, 100, 0) : at(0, scenario == 3 ? 18 : 0, 90)).value();
            auto origin = point;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                auto* coordinate = axis == 0 ? &origin.x : axis == 1 ? &origin.y : &origin.z;
                *coordinate = Fixed::from_raw(coordinate->raw() - drawn.rows[axis][0].raw() * 10);
            }
            if (scenario < 2) {
                const auto distance = [&](const math::Mat3x4& matrix, const math::Vec3 local) {
                    const auto p = math::transform_point(matrix, local).value();
                    return math::length(math::Vec3{Fixed::from_raw(p.x.raw() - origin.x.raw()),
                        Fixed::from_raw(p.y.raw() - origin.y.raw()), Fixed::from_raw(p.z.raw() - origin.z.raw())}).value();
                };
                expect(distance(drawn, target.hardpoints[0].position) < distance(drawn, target.hardpoints[1].position)
                    && distance(level, target.hardpoints[0].position) > distance(level, target.hardpoints[1].position),
                    "ABF-01: rolled and level frames select different nearest hardpoints");
            } else {
                tactical::detail::CombatUnit geometry; geometry.profile = &target; geometry.transform = drawn;
                const auto banked_overlap = tactical::detail::collision_overlaps_sphere(geometry, point, units(5));
                geometry.transform = level;
                const auto level_overlap = tactical::detail::collision_overlaps_sphere(geometry, point, units(5));
                expect(banked_overlap && level_overlap && banked_overlap.value() == (scenario != 3)
                    && level_overlap.value() == (scenario == 3), "ABF-02: rolled and level boxes disagree on sphere overlap");
            }
            if (scenario == 4) origin = point;
            const auto spawned = world.stage_spawn({0, plain_type, 1, origin, math::identity_quat(), {}});
            expect(static_cast<bool>(spawned),
                "ABF-01: ability holder stages next to the rolled bone or box");
            if (!spawned) continue;
            const auto holder_id = spawned.value();
            tactical::AbilityPayload payload{kind, tactical::AbilityAction::activate,
                scenario == 0 ? 2U : eawr::sim::invalid_entity_id, tactical::no_hardpoint};
            if (scenario != 0) payload.position = point;
            if (scenario != 4) {
                const auto submitted = world.submit(command(50, 1, 0, holder_id, payload));
                expect(static_cast<bool>(submitted), "ABF-01: banked ability command submits");
                if (!submitted) std::cerr << submitted.error().message << '\n';
            }
            for (unsigned frame = 0; frame < 3; ++frame) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "ABF-01: banked ability service succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256 + step.value().snapshot->sha256());
                if (frame == 0 && scenario == 0)
                    expect(world.ability_state(holder_id)->slots.front().target_hardpoint == 0,
                        "ABF-01: energy fallback chooses the rolled nearest standing hardpoint");
                if (frame == 0 && (scenario == 2 || scenario == 3)) {
                    const auto instances = world.snapshot()->instances();
                    const auto victim = std::find_if(instances.begin(), instances.end(), [](const auto& instance) { return instance.entity_id == 2; });
                    expect(victim != instances.end() && victim->corrupt.has_value() == (scenario == 2),
                        "ABF-02: collection bounds and area overlap both follow the rolled hull");
                }
            }
            if (scenario < 2) expect(world.durability_state(2)->hardpoints[0] < units(100)
                && world.durability_state(2)->hardpoints[1] == units(100), "ABF-01: banked energy/Buzz damage reaches only the nearest rolled bone");
            if (scenario == 4) expect(world.durability_state(2)->hardpoints[0] > units(50)
                && world.durability_state(2)->hardpoints[1] == units(100), "ABF-02: healing reaches the rolled box outside the planar centre radius");
            if (reference.empty()) reference = hashes;
            else expect(reference == hashes, "ABF-01/02: banked ability state and snapshot equality at 1/2/4/8 workers");
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


// WAB-36 (legacy #2997): the energy weapon and the tractor beam admit an enemy attack target, so a
// player the engine plays switches them on through the attack-state pass with no ability command:
// one per pass, in the nested abilities' authored order. Scenarios: 0 AI owner, energy weapon
// authored first; 1 the same unit owned by a human with autofire off; 2 AI owner, tractor first.
void test_beam_autofire() {
    using K = tactical::AbilityKind;
    for (const unsigned scenario : {0U, 1U, 2U}) {
        std::vector<std::string> baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(100, 0, 100), math::identity_quat(), {}}};
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
            health.profiles[1].hardpoints = {{tactical::HardpointRole::other, true, units(50)},
                {tactical::HardpointRole::other, true, units(100)}};
            tactical::UnitAbilityProfile holder{corvette_type, {}, false};
            std::vector<tactical::SpecialAbilityProfile> nested;
            for (const auto kind : {K::energy_weapon, K::tractor_beam}) {
                tactical::AbilityProfile ordinary;
                ordinary.kind = kind; ordinary.recharge_frames = 20;
                ordinary.expiration_frames = kind == K::energy_weapon ? 30 : 0;
                ordinary.gui_activated_ability_name = kind == K::energy_weapon ? "energy" : "tractor";
                holder.abilities.push_back(ordinary);
                tactical::SpecialAbilityProfile beam;
                beam.name = ordinary.gui_activated_ability_name; beam.style = tactical::SpecialActivationStyle::user_input;
                beam.kind = kind == K::energy_weapon ? tactical::SpecialAbilityKind::energy_weapon
                    : tactical::SpecialAbilityKind::tractor_beam;
                beam.service_interval = 1; beam.beam_min_range = units(5); beam.beam_max_range = units(300);
                beam.damage_per_frame = units(1);
                beam.target_speed_decrease = decimal("0.2"); beam.doubled_speed_categories = 2;
                beam.filter.applicable_categories = 2;
                nested.push_back(beam);
            }
            if (scenario == 2) std::swap(nested[0], nested[1]);
            holder.special = nested;
            tactical::AbilityTable table; table.profiles = {holder}; table.beam_damage_type = 0;
            if (scenario == 1) table.humans = {1};
            tactical::CombatProfile source; source.type_id = corvette_type; source.category_bits = 1;
            tactical::WeaponProfile weapon;
            weapon.range = units(300); weapon.min_recharge_hundredths = 10; weapon.max_recharge_hundredths = 10;
            weapon.cone_width = units(360); weapon.cone_height = units(180);
            weapon.shot = tactical::ShotProfile{units(1), 0, units(1000), units(1000), true, true, {}};
            source.weapons = {weapon}; source.max_attack_distance = units(300);
            tactical::CombatProfile target; target.type_id = frigate_type; target.category_bits = 2;
            target.living_projectile_collision = true;
            target.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
            target.hardpoints = {{0, at(-10, 0), true}, {1, at(10, 0), true}};
            tactical::CombatTable combat; combat.profiles = {source, target};
            const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(1000)}, {frigate_type, units(1000)}};
            auto created = tactical::TacticalSession::create(initial, sensors, health, motion(), std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "WAB-36: beam autofire fixture binds");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            std::array<std::optional<unsigned>, 2> locked{}; // energy, tractor
            for (unsigned frame = 0; frame < 8; ++frame) {
                const auto stepped = world.step(executor);
                expect(static_cast<bool>(stepped), "WAB-36: beam autofire tick succeeds");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                const auto attack = world.combat_state(1);
                const auto slots = world.ability_state(1)->slots;
                for (std::size_t index = 0; index < 2; ++index) {
                    if (!slots[index].active || locked[index]) continue;
                    locked[index] = frame;
                    expect(attack && slots[index].target == attack->attack_target && slots[index].target == 2,
                        "WAB-36: the beam locks onto the unit's own attack target");
                    if (index == 0) expect(slots[index].target_hardpoint == 0,
                        "WHE-59: the autofired energy beam takes the nearest standing targetable hardpoint");
                }
                if (locked[1]) {
                    const auto instances = stepped.value().snapshot->instances();
                    const auto held = std::find_if(instances.begin(), instances.end(), [](const auto& value) { return value.entity_id == 2; });
                    expect(held != instances.end() && held->in_tractor_beam, "WHE-27: the autofired tractor holds its target");
                }
            }
            if (scenario == 1) expect(!locked[0] && !locked[1], "WAB-36: a human's beams stay off without autofire");
            else {
                expect(locked[0] && locked[1], "WAB-36: an AI owner's energy weapon and tractor beam both lock without a command");
                if (locked[0] && locked[1]) {
                    const auto first = scenario == 0 ? *locked[0] : *locked[1];
                    const auto second = scenario == 0 ? *locked[1] : *locked[0];
                    expect(second == first + 1, "WAB-36: one ability per pass, in the nested abilities' authored order");
                }
            }
            const auto record = world.record();
            expect(std::none_of(record.commands.begin(), record.commands.end(), [](const auto& entry) {
                return std::holds_alternative<tactical::AbilityPayload>(entry.payload);
            }), "WAB-36: no ability command is recorded");
            if (baseline.empty()) baseline = hashes;
            else expect(hashes == baseline, "WAB-36: beam autofire hashes equal at 1/2/4/8 workers");
        }
    }
}


// PU-39/WAB-36: an arriving beam craft can receive its team's retaliatory target,
// but the holder pass still waits for the arrival-end tick.
void test_arriving_beam_autofire() {
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = setup();
        initial.units = {{1, station_type, 1, at(-2000, 0), math::identity_quat(), {}},
            {2, plain_type, 2, at(100, 0), math::identity_quat(), {}}};
        auto movement = hangar_motion();
        movement.squadrons.spawners.clear();
        // U-09: tractor admission requires a ship locomotor on its target.
        auto enemy_motion = motion().profiles.front(); enemy_motion.type_id = plain_type;
        movement.profiles = {enemy_motion}; movement.rules = motion().rules;
        auto& squadron = movement.squadrons.squadrons.front();
        squadron.members = {xwing_type}; squadron.offsets = {at(0, 0)};
        squadron.attack_move_response_range = units(10000);
        squadron.autonomous_move_extension_vs_attacker = units(10000);
        auto health = durability();
        health.profiles = {{plain_type, units(10000), {}, false, {}},
            {station_type, units(10000), {}, false, {}}, {xwing_type, units(10000), {}, false, {}}};
        tactical::AbilityProfile beam;
        beam.kind = tactical::AbilityKind::tractor_beam;
        beam.recharge_frames = 20; beam.gui_activated_ability_name = "arrival-tractor";
        auto holder = unit_of(xwing_type, beam);
        tactical::SpecialAbilityProfile nested;
        nested.name = "arrival-tractor"; nested.kind = tactical::SpecialAbilityKind::tractor_beam;
        nested.style = tactical::SpecialActivationStyle::user_input; nested.service_interval = 1;
        nested.beam_max_range = units(10000); nested.filter.applicable_categories = 2;
        holder.special = {nested};
        tactical::AbilityTable abilities; abilities.profiles = {holder};
        tactical::CombatProfile craft; craft.type_id = xwing_type; craft.category_bits = 1;
        craft.max_attack_distance = units(10000);
        tactical::CombatProfile enemy; enemy.type_id = plain_type; enemy.category_bits = 2;
        enemy.max_attack_distance = units(10000);
        enemy.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
        tactical::WeaponProfile weapon;
        weapon.range = units(10000); weapon.min_recharge_hundredths = 10; weapon.max_recharge_hundredths = 10;
        weapon.cone_width = units(360); weapon.cone_height = units(180);
        weapon.shot = tactical::ShotProfile{units(1), 0, units(10000), units(10000), true, true, {}};
        enemy.weapons = {weapon};
        craft.weapons = {weapon};
        craft.collision = enemy.collision;
        tactical::CombatTable combat; combat.profiles = {enemy, craft}; combat.unit_ai_types = {xwing_squadron};
        auto team = craft; team.type_id = xwing_squadron; team.weapons.clear();
        combat.retaliation_team_types = {team};
        tactical::EconomyRules economy;
        economy.players = {{1, units(100), 25, false, {}, 1, 3}};
        tactical::BuildOption option;
        option.type = xwing_squadron; option.price = units(10);
        option.build_frames = 1; option.ai_build_frames = 1; option.available = true;
        economy.menus = {{station_type, 1, {option}}};
        economy.footprints = {{xwing_squadron, std::nullopt, {}}};
        const std::vector<tactical::SensorProfile> sensors{{plain_type, units(10000)}, {xwing_type, units(10000)}};
        auto created = tactical::TacticalSession::create(initial, sensors, health, movement, std::nullopt,
            combat, {}, abilities, economy);
        expect(static_cast<bool>(created), "PU-39: arriving beam fixture creates");
        if (!created) { std::cerr << created.error().message << '\n'; continue; }
        auto world = std::move(created).value();
        expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, tactical::BuyPayload{xwing_squadron}))),
            "PU-39: beam squadron purchase submits");
        constexpr unsigned arrival_start = 2;
        expect(static_cast<bool>(world.submit({{arrival_start, 1, 1}, {}, tactical::ReinforcePayload{xwing_squadron, at(0, 0)}})),
            "PU-39: beam squadron reinforcement submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::optional<unsigned> first_lock;
        bool propagated_in_lane = false;
        for (unsigned tick = 0; tick <= arrival_start + tactical::arrival_frames; ++tick) {
            // The reinforcement allocates craft 3, then team container 4. A hit on the
            // visible craft changes the team target and propagates it back to the roster.
            if (tick == arrival_start + tactical::arrival_visible_frame)
                expect(static_cast<bool>(world.submit(command(tick, 2, 0, 2, tactical::AttackPayload{3}))),
                    "PU-39: enemy attacks the visible arriving craft");
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "PU-39: arriving beam fixture steps");
            if (!step) { std::cerr << step.error().message << '\n'; break; }
            hashes.push_back(step.value().state_sha256);
            const auto state = world.ability_state(3);
            if (!state) continue;
            const auto attack = world.combat_state(3);
            const auto mind = world.squadron_state(4);
            if (world.arrivals().contains(3) && attack && mind && attack->attack_target == 2 && mind->target == 2) {
                const auto live = world.units();
                const auto source = std::find_if(live.begin(), live.end(), [](const auto& unit) { return unit.entity_id == 3; });
                const auto target = std::find_if(live.begin(), live.end(), [](const auto& unit) { return unit.entity_id == 2; });
                expect(source != live.end() && target != live.end()
                    && tactical::within_range(source->position, target->position, nested.beam_max_range, tactical::RangeMetric::planar),
                    "PU-39: propagated team target is in beam range during arrival");
                propagated_in_lane = true;
            }
            if (tick < arrival_start + tactical::arrival_frames)
                expect(!state->slots[0].active, "PU-39: an arriving holder never locks its propagated target");
            if (state->slots[0].active && !first_lock) first_lock = tick;
        }
        expect(propagated_in_lane, "PU-39: fixture witnesses a propagated team target inside the arrival lane");
        expect(first_lock && *first_lock == arrival_start + tactical::arrival_frames,
            "PU-39: first beam lock is exactly the arrival-end tick");
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "PU-39: arriving beam hashes equal at 1/2/4/8 workers");
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


// #942: the stored roster keeps destroyed escort IDs; WHE-64 shares count only living members, and a hero
// whose escorts are all destroyed takes its own ordinary damage until a refill.
void test_hero_damage_after_escorts_die() {
    constexpr tactical::TypeId escort = 23;
    std::vector<std::string> reference;
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
        auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, combat, {}, {});
        expect(static_cast<bool>(created), "#942 escort-death fixture creates");
        if (!created) continue;
        auto world = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const auto frame = [&] {
            auto step = world.step(executor); expect(static_cast<bool>(step), "#942 escort-death frame completes");
            if (step) hashes.push_back(step.value().state_sha256);
            return static_cast<bool>(step);
        };
        // Every frame the leader stays in the field and takes one ordinary 60-unit environmental hit.
        if (!frame()) continue;
        expect(world.durability_state(11)->hull == units(1000) && world.durability_state(13)->hull == units(970),
            "WHE-64 two living escorts split the hit; the hero stays intact");
        expect(static_cast<bool>(world.submit(command(1, 1, 0, 12, tactical::DamagePayload{units(2000), tactical::hull_target}))),
            "#942 first escort kill submits");
        if (!frame()) continue;
        expect(world.squadrons()[0].members.size() == 2, "#942 first destroyed escort is pruned");
        const auto before = world.durability_state(13)->hull;
        if (!frame()) continue;
        expect(world.durability_state(11)->hull == units(1000)
            && world.durability_state(13)->hull == Fixed::from_raw(before.raw() - units(60).raw()),
            "#942 the share count is the living members: the last escort takes the whole hit");
        expect(static_cast<bool>(world.submit(command(3, 1, 0, 13, tactical::DamagePayload{units(2000), tactical::hull_target}))),
            "#942 last escort kill submits");
        if (!frame() || !frame()) continue;
        expect(world.squadrons()[0].members.size() == 1, "#942 both escorts are pruned from the squadron");
        expect(world.durability_state(11)->hull < units(1000), "#942 hero with no living escort takes its own damage");
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "#942 escort-death routing equal at 1/2/4/8 workers");
    }
}

} // namespace ability_tests
