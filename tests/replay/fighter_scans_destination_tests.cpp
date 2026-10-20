#include "fighter_support.hpp"
#include "fighter_scans_fixtures.hpp"
#include "eawr/sim/tactical/fog_cells.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fighter_test_support {

void test_attack_order_target_loss_hold() {
    // FD-09: an ordinary attack base is not an unfinished escort hold.
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(1, carrier_type, empire, at(-2000, 0)),
            unit(10, squadron_b, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0)),
            unit(20, frigate_type, rebel, at(1500, 0))};
        setup.squadrons = {{10, {11}}};
        auto table = motion();
        table.squadrons.spawners.clear();
        auto made = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
        expect(static_cast<bool>(made), "FD-09: distant attack-order loss fixture builds");
        if (!made) return;
        auto session = std::move(made).value();
        // Start from a player escort, then replace it with an ordinary attack.
        // The pending escort discriminator must not survive the replacement.
        expect(static_cast<bool>(session.submit({{0, empire, 1}, {10}, tactical::GuardPayload{{}, 1}})),
            "FD-09: player escort submits before its replacement attack");
        expect(static_cast<bool>(session.submit({{0, empire, 2}, {10}, tactical::AttackPayload{20}})),
            "FD-09: replacement attack order submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        math::Vec3 order_point{};
        std::uint64_t last_tick = 0;
        bool departed = false;
        const auto reach = units(tactical::idle_cell_size);
        for (int tick = 0; tick < 240 && !departed; ++tick) {
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "FD-09: attack approaches its distant target");
            if (!step) return;
            hashes.push_back(step.value().state_sha256);
            last_tick = step.value().completed_tick;
            const auto state = session.squadron_state(10);
            const auto leader = instance(*step.value().snapshot, 11);
            expect(state && state->formation && !state->formation->complete && state->approach
                && state->escorted == 0 && state->target == 20,
                "FD-09: an incomplete attack base has no retained escort identity");
            if (!state || !leader) return;
            order_point = state->anchor;
            departed = !tactical::within_range(position(*leader), order_point, units(360), tactical::RangeMetric::planar);
        }
        expect(departed, "FD-09: target loss is tested well beyond the attack order point");
        if (!departed) return;
        expect(static_cast<bool>(session.submit({{last_tick, empire, 3}, {20}, tactical::DamagePayload{units(6000)}})),
            "FD-09: target dies before attack formation completion");
        const auto damaged = session.step(executor);
        expect(static_cast<bool>(damaged), "FD-09: target damage advances");
        if (!damaged) return;
        hashes.push_back(damaged.value().state_sha256);
        const auto before = session.squadron_state(10);
        const auto leader = instance(*damaged.value().snapshot, 11);
        expect(before && before->formation && !before->formation->complete && before->approach && leader,
            "FD-09: target is removed while the approach and incomplete base remain");
        if (!before || !leader) return;
        const auto combat_end_point = position(*leader);
        const auto ended = session.step(executor);
        expect(static_cast<bool>(ended), "FD-09: dead attack target cleanup advances");
        if (!ended) return;
        hashes.push_back(ended.value().state_sha256);
        const auto held = session.squadron_state(10);
        expect(held && held->mode == tactical::SquadronMode::idle && held->target == 0 && !held->approach
            && held->escorted == 0 && held->idle_cell && held->formation
            && held->formation->base_target == 0 && held->anchor != order_point
            && tactical::within_range(held->anchor, combat_end_point, reach, tactical::RangeMetric::planar),
            "FD-09: attack target death claims the combat-end hold instead of returning to its order point");
        if (!held) return;
        const auto end_hold = held->anchor;
        const auto saved = session.save_checkpoint_state();
        auto copy = tactical::TacticalSession::from_replay(session.record(), sensors(), durability(), table,
            std::nullopt, combat());
        expect(saved && copy && copy.value().load_checkpoint_state(saved.value()),
            "FD-09: combat-end attack hold restores without a format change");
        if (!saved || !copy) return;
        auto continued = std::move(copy).value();
        for (int tick = 0; tick < 30; ++tick) {
            const auto step = session.step(executor);
            const auto resumed = continued.step(executor);
            expect(step && resumed && step.value().state_sha256 == resumed.value().state_sha256,
                "FD-09: restored attack hold keeps identical subsequent hashes");
            if (!step || !resumed) return;
            hashes.push_back(step.value().state_sha256);
            const auto state = session.squadron_state(10);
            expect(state && state->anchor == end_hold && state->idle_cell,
                "FD-09: the attack holds at combat end after target cleanup");
        }
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "FD-09: attack-order target loss matches on 1/2/4/8 workers");
    }
}

void test_lost_launch_combat_completion() {
    // WMV-18/FD-09: ending combat after carrier loss must keep the pending
    // escort hold, rather than claiming an idle cell at the current position.
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(1, carrier_type, empire, at(0, 0)), unit(10, frigate_type, rebel, at(650, 0))};
        auto table = motion();
        auto& hangar = table.squadrons.spawners.front();
        hangar.mobile = true;
        hangar.entries = {{squadron_a, 1, 0}};
        hangar.bays.front().position = at(600, 0);
        auto made = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
        expect(static_cast<bool>(made), "WMV-18: hostile-plus-far-launch fixture builds");
        if (!made) return;
        auto session = std::move(made).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        eawr::sim::EntityId reserve = 0;
        std::uint64_t last_tick = 0;
        bool acquired = false;
        for (int tick = 0; tick < 120 && !acquired; ++tick) {
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "WMV-18: escort acquires a hostile before carrier loss");
            if (!step) return;
            last_tick = step.value().completed_tick;
            hashes.push_back(step.value().state_sha256);
            if (!session.squadrons().empty()) reserve = session.squadrons().front().container;
            const auto held = session.squadron_state(reserve);
            acquired = held && held->target == 10;
        }
        expect(acquired, "WMV-18: the pre-arrival escort actually holds the hostile");
        if (!acquired) return;
        const auto fighting = session.squadron_state(reserve);
        const auto retained_hold = fighting->anchor;
        const auto reach = units(tactical::idle_cell_size);
        const auto leader = instance(*session.snapshot(), fighting->roster.front());
        expect(leader && !tactical::within_range(position(*leader), retained_hold, reach, tactical::RangeMetric::planar)
            && !fighting->idle_cell && fighting->formation && !fighting->formation->complete
            && !fighting->formation->has_reached_done, "WMV-18: combat begins while the launch is far from its hold");
        expect(static_cast<bool>(session.submit({{last_tick, empire, 1}, {1},
            tactical::DamagePayload{units(6000)}})), "WMV-18: fighting escort loses its carrier before arrival");
        const auto damage_carrier = session.step(executor);
        expect(static_cast<bool>(damage_carrier), "WMV-18: carrier damage command advances");
        if (!damage_carrier) return;
        hashes.push_back(damage_carrier.value().state_sha256);
        const auto loss = session.step(executor);
        expect(static_cast<bool>(loss), "WMV-18: fighting escort carrier loss advances");
        if (!loss) return;
        hashes.push_back(loss.value().state_sha256);
        const auto orphan = session.squadron_state(reserve);
        expect(orphan && orphan->mode == tactical::SquadronMode::idle && orphan->target == 10
            && orphan->escorted == 1
            && !orphan->idle_cell && orphan->anchor == retained_hold && orphan->formation
            && orphan->formation->base_target == 0 && !orphan->formation->complete,
            "WMV-18: lost escort retains its unfinished hold while combat continues");
        expect(static_cast<bool>(session.submit({{loss.value().completed_tick, empire, 2}, {10},
            tactical::DamagePayload{units(6000)}})), "WMV-18: hostile dies after the carrier and before hold arrival");
        const auto damage_hostile = session.step(executor);
        expect(static_cast<bool>(damage_hostile), "WMV-18: hostile damage command advances");
        if (!damage_hostile) return;
        hashes.push_back(damage_hostile.value().state_sha256);
        const auto ended = session.step(executor);
        expect(static_cast<bool>(ended), "WMV-18: dead-target combat cleanup advances");
        if (!ended) return;
        hashes.push_back(ended.value().state_sha256);
        const auto pending = session.squadron_state(reserve);
        const auto pending_leader = pending ? instance(*ended.value().snapshot, pending->roster.front()) : nullptr;
        expect(pending && pending_leader && pending->target == 0 && pending->anchor == retained_hold
            && pending->escorted == 1
            && !pending->idle_cell && pending->formation && !pending->formation->attack_override
            && !pending->formation->complete && !pending->formation->has_reached_done
            && !tactical::within_range(position(*pending_leader), retained_hold, reach, tactical::RangeMetric::planar),
            "WMV-18: ending combat far from the retained hold cannot complete the lost escort");
        const auto saved = session.save_checkpoint_state();
        auto copy = tactical::TacticalSession::from_replay(session.record(), sensors(), durability(), table,
            std::nullopt, combat());
        expect(saved && copy && copy.value().load_checkpoint_state(saved.value()),
            "WMV-18: post-combat pending hold restores without a format change");
        if (!saved || !copy) return;
        auto continued = std::move(copy).value();
        bool completed = false;
        for (int tick = 0; tick < 900 && !completed; ++tick) {
            const auto step = session.step(executor);
            const auto resumed = continued.step(executor);
            expect(step && resumed && step.value().state_sha256 == resumed.value().state_sha256,
                "WMV-18: post-combat pending hold continues with identical checkpoint hashes");
            if (!step || !resumed) return;
            hashes.push_back(step.value().state_sha256);
            const auto held = session.squadron_state(reserve);
            expect(held && held->formation && held->formation->complete == held->idle_cell.has_value()
                && held->formation->has_reached_done == held->idle_cell.has_value(),
                "WMV-18: post-combat completion waits for a successful retained-hold cell claim");
            if (!held || !held->formation) return;
            completed = held->formation->complete;
            expect(held->escorted == (completed ? 0U : 1U),
                "WMV-18: pending escort provenance is retained through restore and cleared on completion");
            const auto current_leader = instance(*step.value().snapshot, held->roster.front());
            if (completed) expect(current_leader && tactical::within_range(position(*current_leader), retained_hold,
                reach, tactical::RangeMetric::planar), "WMV-18: combat cleanup completes only at actual retained-hold arrival");
            else expect(held->anchor == retained_hold, "WMV-18: pending post-combat flight preserves the retained hold");
        }
        expect(completed, "WMV-18: the post-combat orphan eventually reaches its hold and completes");
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "WMV-18: carrier loss then target death match on 1/2/4/8 workers");
    }
}

void test_launch_completion() {
    // WMV-18: FM-24's arrived escort hold stands in for its base path.
    // A remote bay isolates birth from arrival without changing authored reach.
    std::vector<std::string> reference, orphan_reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(1, carrier_type, empire, at(0, 0))};
        auto table = motion();
        auto& hangar = table.squadrons.spawners.front();
        hangar.mobile = true;
        hangar.entries = {{squadron_a, 1, 0}};
        hangar.bays.front().position = at(600, 0);
        auto made = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
        expect(static_cast<bool>(made), "WMV-18: far mobile-launch fixture builds");
        if (!made) return;
        auto session = std::move(made).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::optional<tactical::TacticalSession> continued;
        eawr::sim::EntityId reserve = 0;
        bool arrived = false;
        std::uint64_t last_tick = 0;
        for (int tick = 0; tick < 900 && !arrived; ++tick) {
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "WMV-18: far launch and arrival advance");
            if (!step) return;
            last_tick = step.value().completed_tick;
            hashes.push_back(step.value().state_sha256);
            if (continued) {
                const auto resumed = continued->step(executor);
                expect(resumed && resumed.value().state_sha256 == step.value().state_sha256,
                    "WMV-18: launch-arrival checkpoint matches every uninterrupted hash");
            }
            if (reserve == 0 && !session.squadrons().empty()) {
                reserve = session.squadrons().front().container;
                const auto born = session.squadron_state(reserve);
                expect(born && born->formation && !born->formation->complete && !born->formation->has_reached_done
                    && !born->idle_cell, "WMV-18: a far mobile launch does not complete at birth");
                const auto saved = session.save_checkpoint_state();
                auto copy = tactical::TacticalSession::from_replay(session.record(), sensors(), durability(), table,
                    std::nullopt, combat());
                expect(saved && copy, "WMV-18: unfinished launch checkpoint and replay exist");
                if (!saved || !copy) return;
                continued.emplace(std::move(copy).value());
                const auto loaded = continued->load_checkpoint_state(saved.value());
                expect(static_cast<bool>(loaded), "WMV-18: unfinished launch restores without a format change");
                if (!loaded) return;
                expect(continued->canonical_state_bytes() == session.canonical_state_bytes(),
                    "WMV-18: restored unfinished launch has identical canonical state");
                auto probe = tactical::TacticalSession::from_replay(session.record(), sensors(), durability(), table,
                    std::nullopt, combat());
                expect(probe && probe.value().load_checkpoint_state(saved.value()),
                    "WMV-18: unfinished launch can fork an active-approach control");
                if (!probe) return;
                const auto distant = probe.value().stage_spawn(unit(0, frigate_type, rebel, at(6000, 0)));
                expect(distant && probe.value().submit({{last_tick + 1, empire, 1}, {reserve},
                    tactical::AttackPayload{distant.value()}}), "WMV-18: fresh launch accepts a distant attack");
                if (!distant) return;
                bool approaching = false;
                for (int visit = 0; visit < 30; ++visit) {
                    const auto advance = probe.value().step(executor);
                    expect(static_cast<bool>(advance), "WMV-18: active approach control advances");
                    if (!advance) return;
                    const auto held = probe.value().squadron_state(reserve);
                    approaching = approaching || (held && held->approach);
                    expect(held && held->formation && !held->formation->complete && !held->formation->has_reached_done,
                        "WMV-18: active launch approach cannot synthesize completion history");
                }
                expect(approaching, "WMV-18: the negative control actually has an active approach");
                auto orphan_made = tactical::TacticalSession::from_replay(session.record(), sensors(), durability(), table,
                    std::nullopt, combat());
                expect(orphan_made && orphan_made.value().load_checkpoint_state(saved.value()),
                    "WMV-18: far launch forks a pre-arrival carrier-loss control");
                if (!orphan_made) return;
                auto orphan = std::move(orphan_made).value();
                expect(static_cast<bool>(orphan.submit({{last_tick + 1, empire, 1}, {1},
                    tactical::DamagePayload{units(6000)}})), "WMV-18: destroy carrier before escort arrival");
                std::vector<std::string> orphan_hashes;
                for (int visit = 0; visit < 3; ++visit) {
                    const auto advance = orphan.step(executor);
                    expect(static_cast<bool>(advance), "WMV-18: early carrier loss advances");
                    if (!advance) return;
                    orphan_hashes.push_back(advance.value().state_sha256);
                }
                const auto unarrived = orphan.squadron_state(reserve);
                expect(unarrived && !unarrived->idle_cell && unarrived->formation
                    && !unarrived->formation->complete && !unarrived->formation->has_reached_done,
                    "WMV-18: carrier death cannot complete an unarrived far launch");
                const auto loss_saved = orphan.save_checkpoint_state();
                auto loss_copy = tactical::TacticalSession::from_replay(orphan.record(), sensors(), durability(), table,
                    std::nullopt, combat());
                expect(loss_saved && loss_copy && loss_copy.value().load_checkpoint_state(loss_saved.value()),
                    "WMV-18: pending hold survives an early-loss checkpoint");
                if (!loss_saved || !loss_copy) return;
                auto loss_continued = std::move(loss_copy).value();
                bool completed = false;
                for (int visit = 0; visit < 900 && !completed; ++visit) {
                    const auto advance = orphan.step(executor);
                    const auto resumed = loss_continued.step(executor);
                    expect(advance && resumed && advance.value().state_sha256 == resumed.value().state_sha256,
                        "WMV-18: pending lost-escort arrival continues identically");
                    if (!advance || !resumed) return;
                    orphan_hashes.push_back(advance.value().state_sha256);
                    const auto held = orphan.squadron_state(reserve);
                    expect(held && held->formation && held->formation->complete == held->idle_cell.has_value()
                        && held->formation->has_reached_done == held->idle_cell.has_value(),
                        "WMV-18: a lost far escort completes only after its retained hold cell is reached");
                    completed = held && held->formation && held->formation->complete;
                }
                expect(completed, "WMV-18: the orphaned launch reaches its retained hold and completes later");
                if (workers == 1) orphan_reference = orphan_hashes;
                else expect(orphan_hashes == orphan_reference,
                    "WMV-18: pre-arrival loss and later completion match on 1/2/4/8 workers");
            }
            const auto mind = session.squadron_state(reserve);
            if (!mind || !mind->formation) continue;
            arrived = mind->idle_cell.has_value();
            expect(mind->formation->complete == arrived && mind->formation->has_reached_done == arrived,
                "WMV-18: completion is withheld until escort arrival, then records base history");
        }
        expect(reserve != 0 && arrived, "WMV-18: the far launched squadron reaches its carrier and completes");
        if (!arrived) return;
        // Carrier destruction is an ordinary command; introduce the enemy only
        // after escort loss so its scan must use the idle completion history.
        expect(static_cast<bool>(session.submit({{last_tick + 1, empire, 1}, {1},
            tactical::DamagePayload{units(6000)}})), "WMV-18: carrier destruction submits");
        for (int tick = 0; tick < 3; ++tick) {
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "WMV-18: carrier loss advances");
            if (!step) return;
            hashes.push_back(step.value().state_sha256);
        }
        const auto lost = session.squadron_state(reserve);
        expect(lost && lost->mode == tactical::SquadronMode::idle && lost->formation
            && lost->formation->base_target == 0 && lost->formation->has_reached_done,
            "WMV-18: carrier loss retains reached-done history on the converted idle base");
        const auto enemy = session.stage_spawn(unit(0, frigate_type, rebel, at(650, 0)));
        expect(static_cast<bool>(enemy), "WMV-18: beyond-own-reach enemy stages after carrier loss");
        if (!enemy) return;
        bool acquired = false, pursued = false, fired = false;
        for (int tick = 0; tick < 200; ++tick) {
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "WMV-18: completed launch diversion advances");
            if (!step) return;
            hashes.push_back(step.value().state_sha256);
            const auto mind = session.squadron_state(reserve);
            acquired = acquired || (mind && mind->target == enemy.value());
            pursued = pursued || (mind && mind->formation && mind->formation->attack_target == enemy.value());
            for (const auto& event : step.value().snapshot->combat_events())
                fired = fired || (mind && event.kind == tactical::CombatEventKind::weapon_fired
                    && std::find(mind->roster.begin(), mind->roster.end(), event.shooter) != mind->roster.end());
        }
        expect(acquired && pursued && fired, "WMV-18: actual launch completion admits later idle diversion and firing");
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "WMV-18: actual launch, completion and diversion match on 1/2/4/8 workers");
    }
}

void test_player_escort_completion() {
    for (const bool lose_base : {false, true}) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.seed = seed;
            setup.players = players();
            setup.units = {unit(1, carrier_type, empire, at(0, 0)), unit(10, squadron_a, empire, at(600, 0)),
                unit(11, craft_type, empire, at(600, 0)), unit(12, craft_type, empire, at(590, 10))};
            setup.squadrons = {{10, {11, 12}}};
            auto table = motion();
            table.squadrons.spawners.clear();
            auto made = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
            expect(static_cast<bool>(made), "WMV-18: player escort fixture builds");
            if (!made) return;
            auto session = std::move(made).value();
            expect(static_cast<bool>(session.submit({{0, empire, 1}, {10}, tactical::GuardPayload{at(0, 0), 1}})),
                "WMV-18: player guard-of-unit installs an escort base");
            if (lose_base) expect(static_cast<bool>(session.submit({{30, empire, 2}, {1},
                tactical::DamagePayload{units(6000)}})), "WMV-18: player escort base is lost before arrival");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            bool completed = false;
            for (int tick = 0; tick < 900 && !completed; ++tick) {
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), "WMV-18: player escort arrival advances");
                if (!step) return;
                hashes.push_back(step.value().state_sha256);
                const auto held = session.squadron_state(10);
                if (tick == 20 || (lose_base && tick == 35)) expect(held && !held->idle_cell && held->formation
                    && !held->formation->complete && held->formation->has_reached_done,
                    "WMV-18: an unfinished player escort keeps prior history but does not complete on loss");
                completed = tick > 0 && held && held->formation && held->formation->complete;
                if (completed) expect(held->idle_cell.has_value(),
                    "WMV-18: generic player escort completion is sourced from hold-cell arrival");
            }
            expect(completed, "WMV-18: player escort completes at its live or retained hold");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WMV-18: player escort arrival/loss match on 1/2/4/8 workers");
        }
    }
}

void test_base_destination_adoption() {
    // WSQ-41: the base object's direct adoption precedes the nearby fighter's better
    // scan priority. Admission uses the base itself, not an autonomous top override.
    for (int mode = 0; mode != 12; ++mode) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.seed = seed;
            setup.players = players();
            setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0)),
                unit(12, craft_type, empire, at(-10, 10)),
                unit(20, frigate_type, rebel, at(mode == 5 ? 1400 : mode == 8 ? 100
                    : mode == 9 ? 700 : mode == 10 ? 510 : 450, 0)),
                unit(21, craft_type, rebel, at(200, 0)), unit(30, frigate_type, empire, at(400, 0))};
            setup.squadrons = {{10, {11, 12}}};
            auto table = motion();
            table.squadrons.spawners.clear();
            auto guns = combat();
            guns.priority_sets[0].rows = {{craft_type, units(1)}, {frigate_type, units(3)}};
            // #3922: mode 11 is the stock pad case: an attack order on a valid-type object without
            // living-projectile contact reaches the team, but no member takes it as a direct target.
            if (mode == 6 || mode == 11) for (auto& profile : guns.profiles)
                if (profile.type_id == frigate_type) profile.living_projectile_collision = false;
            if (mode == 7) guns.priority_sets[0].rows[1].priority.reset();
            if (mode == 8) for (auto& profile : guns.profiles)
                if (profile.type_id == craft_type) profile.min_attack_distance = units(150);
            auto health = durability();
            if (mode == 9 || mode == 10) {
                // No hardpoint is named by the order: neither a nearer nor a farther
                // firing point replaces the centre in the adoption range test.
                for (auto& profile : guns.profiles) if (profile.type_id == frigate_type) {
                    profile.collision = tactical::CollisionBox{at(-20, -10, -10), at(20, 10, 10)};
                    profile.hardpoints = {{0, at(mode == 9 ? -300 : 500, 0), true}};
                }
                for (auto& profile : health.profiles) if (profile.type_id == frigate_type) {
                    tactical::HardpointProfile hardpoint;
                    hardpoint.destroyable = true;
                    hardpoint.max_health = units(100);
                    profile.hardpoints = {hardpoint};
                }
            }
            auto created = tactical::TacticalSession::create(setup, sensors(), health, table, std::nullopt, guns);
            expect(static_cast<bool>(created), "WSQ-41 base adoption creates");
            if (!created) return;
            auto session = std::move(created).value();
            tactical::CommandPayload command = tactical::AttackMovePayload{{}, 20};
            if (mode == 1 || mode == 11) command = tactical::AttackPayload{20};
            if (mode == 2) command = tactical::AttackMovePayload{at(450, 0), 0};
            if (mode == 3) command = tactical::GuardPayload{{}, 20};
            if (mode == 4) command = tactical::AttackMovePayload{{}, 30};
            expect(static_cast<bool>(session.submit({{0, empire, 1}, {10}, command})), "WSQ-41 base adoption order submits");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (int frame = 0; frame != 5; ++frame) {
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), "WSQ-41 base adoption steps");
                if (!step) return;
                hashes.push_back(step.value().state_sha256);
            }
            const auto member = session.combat_state(12);
            const bool protected_base = mode < 2 || mode == 7 || mode == 10;
            // Minimum reach gates direct formation adoption. Its refusal does not
            // require ordinary targeting to choose a different object afterward.
            expect(member && (mode == 8 || member->attack_target == (protected_base ? 20U : 21U))
                && member->direct == protected_base,
                "WSQ-41: hostile non-escort object base adopted before autonomous scan; point/friendly/guard excluded");
            if (mode == 11) {
                // The members keep scanning (the existing check above names the craft they find); the
                // leader, whose own state is not asserted elsewhere, must not hold the object either.
                const auto leader = session.combat_state(11);
                expect(leader && leader->attack_target != 20 && !leader->direct,
                    "#3922: a noncollidable attack target is never shared to the leader craft");
            }
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WSQ-41 base adoption hashes match on 1/2/4/8 workers");
        }
    }
}

void test_base_destination_lifecycle() {
    enum class Change { move, attack_move, attack, stop, face, move_then_face, death, idle_acquisition, guard };
    for (const auto change : {Change::move, Change::attack_move, Change::attack, Change::stop,
             Change::face, Change::move_then_face, Change::death, Change::idle_acquisition, Change::guard}) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.seed = seed;
            setup.players = players();
            setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0)),
                unit(12, craft_type, empire, at(-10, 10)), unit(20, frigate_type, rebel, at(450, 0)),
                unit(30, frigate_type, empire, at(400, 0))};
            if (change == Change::attack_move || change == Change::attack)
                setup.units.push_back(unit(40, frigate_type, rebel, at(-300, 0)));
            setup.squadrons = {{10, {11, 12}}};
            auto table = motion();
            table.squadrons.spawners.clear();
            auto made = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
            expect(static_cast<bool>(made), "WSQ-41 lifecycle creates");
            if (!made) return;
            auto session = std::move(made).value();
            if (change != Change::idle_acquisition) {
                const tactical::CommandPayload initial_order = change == Change::attack
                    ? tactical::CommandPayload{tactical::AttackPayload{20}}
                    : tactical::CommandPayload{tactical::AttackMovePayload{{}, 20}};
                expect(static_cast<bool>(session.submit({{0, empire, 1}, {10}, initial_order})),
                    "WSQ-41 lifecycle initial object attack submits");
            }
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            const auto advance = [&]() {
                const auto result = session.step(executor);
                expect(static_cast<bool>(result), "WSQ-41 lifecycle advances");
                if (result) hashes.push_back(result.value().state_sha256);
                return static_cast<bool>(result);
            };
            for (int frame = 0; frame < 5; ++frame) if (!advance()) return;
            const auto initial = session.combat_state(12);
            const auto initial_team = session.squadron_state(10);
            expect(initial && initial->attack_target == 20 && initial->direct == (change != Change::idle_acquisition),
                "WSQ-41 lifecycle distinguishes a protected base from an autonomous idle find");
            if (change != Change::idle_acquisition && change != Change::attack)
                expect(initial_team && initial_team->mode == tactical::SquadronMode::escort,
                    "WSQ-41: the model's object attack-move uses escort mode");
            if (change == Change::idle_acquisition)
                expect(initial_team && initial_team->formation && initial_team->formation->base_target == 0,
                    "WSQ-41: an autonomous idle find has no object base to promote");
            const bool moving = change == Change::move || change == Change::move_then_face;
            if (moving)
                expect(static_cast<bool>(session.submit({{5, empire, 2}, {10}, tactical::MovePayload{at(-900, 0)}})),
                    "FO-03: replacement move submits");
            if (change == Change::face || change == Change::move_then_face)
                expect(static_cast<bool>(session.submit({{5, empire, 3}, {10}, tactical::FacePayload{at(0, 500)}})),
                    "FO-03: face submits without replacing the retained base");
            if (change == Change::attack_move)
                expect(static_cast<bool>(session.submit({{5, empire, 2}, {10}, tactical::AttackMovePayload{{}, 40}})),
                    "FO-05: replacement object attack-move submits");
            if (change == Change::attack)
                expect(static_cast<bool>(session.submit({{5, empire, 2}, {10}, tactical::AttackPayload{40}})),
                    "FO-03: explicit attack A-to-B submits");
            if (change == Change::stop)
                expect(static_cast<bool>(session.submit({{5, empire, 2}, {10}, tactical::StopPayload{}})),
                    "FO-03: stop after adoption submits");
            if (change == Change::guard)
                expect(static_cast<bool>(session.submit({{5, empire, 2}, {10}, tactical::GuardPayload{{}, 30}})),
                    "FO-05: replacement friendly escort submits");
            if (change == Change::death)
                expect(static_cast<bool>(session.stage_remove(20)), "WSQ-44: adopted base removal stages");
            bool arrived = false;
            for (int frame = 0; frame < (moving ? 600 : 90); ++frame) {
                if (!advance()) return;
                if (frame < 2) continue; // commands enter after targeting, then reach the next member turn
                const auto member = session.combat_state(12);
                const auto team = session.squadron_state(10);
                arrived = arrived || (team && team->formation && team->formation->complete);
                if (moving || change == Change::death)
                    expect(member && !member->direct && member->attack_target == 0,
                        "FO-03/WSQ-44: replacement or death cannot re-share the prior adopted target");
                else if (change == Change::attack_move || change == Change::attack)
                    expect(member && member->direct && member->attack_target == 40,
                        "FO-05: the new object base replaces the previous direct adoption");
                else if (change == Change::face)
                    expect(member && member->direct && member->attack_target == 20,
                        "FO-03: face preserves the adopted base and direct target");
                else expect(member && !member->direct,
                    "WSQ-41: idle acquisition, stop and a friendly guard remain autonomous");
            }
            if (moving) expect(arrived, "FO-03: the replacement move is checked through arrival");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WSQ-41 lifecycle hashes match on 1/2/4/8 workers");
        }
    }
}

void test_base_destination_release() {
    // WSQ-41: adoption has no visibility/weapon/priority prefilter. Fog loss
    // clears and returns without a scan; a visible unattackable direct target is
    // retained. An ineligible base permits the ordinary deadline; reveal keeps
    // the now-visible base direct without scanning.
    // An independent fixed sensor keeps the alternative visible while the base
    // stays hidden, even when the craft approaches it. Cases release by stop,
    // removal, or visibility alone; none needs a scan-clock reset.
    for (int scenario = 0; scenario < 4; ++scenario) {
        const bool fogged = scenario != 0;
        const bool reveal_base = scenario == 2;
        const bool remove_base = scenario == 3;
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.seed = seed;
            setup.players = players();
            setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0)),
                unit(12, craft_type, empire, at(-10, 10)), unit(20, frigate_type, rebel, at(450, 0)),
                unit(21, craft_type, rebel, at(100, 0))};
            if (fogged) setup.units.push_back(unit(30, frigate_type, empire, at(100, 0)));
            setup.squadrons = {{10, {11, 12}}};
            auto table = motion();
            table.squadrons.spawners.clear();
            auto guns = combat();
            guns.priority_sets.front().rows = {{craft_type, units(1)}, {frigate_type, std::nullopt}};
            if (!fogged) for (auto& profile : guns.profiles) if (profile.type_id == craft_type)
                profile.weapons.front().category_restrictions = 4; // base category; the alternative fighter remains fireable
            std::optional<tactical::FogRules> fog;
            if (fogged) {
                tactical::FogRules rules;
                rules.map_left = units(-2000); rules.map_top = units(2000);
                rules.cell_size = units(100); rules.cells_wide = 40; rules.cells_tall = 40;
                rules.ramp_down_step = tactical::fog_ramp_down_step(units(6)).value();
                fog = rules;
            }
            auto made = tactical::TacticalSession::create(setup,
                fogged ? std::vector<tactical::SensorProfile>{{frigate_type, units(50)}} : sensors(),
                durability(), table, fog, guns);
            expect(static_cast<bool>(made), "WSQ-41 release fixture creates");
            if (!made) return;
            auto session = std::move(made).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (int frame = 0; frame < 5; ++frame) {
                const auto result = session.step(executor);
                expect(static_cast<bool>(result), "WSQ-41: a prior autonomous scan seeds a real deadline");
                if (!result) return;
                hashes.push_back(result.value().state_sha256);
            }
            expect(static_cast<bool>(session.submit({{5, empire, 1}, {10}, tactical::AttackMovePayload{{}, 20}})),
                "WSQ-41: hidden/unattackable object base submits after a prior scan");
            std::optional<std::uint64_t> deadline;
            for (int frame = 0; frame < 60; ++frame) {
                const auto result = session.step(executor);
                expect(static_cast<bool>(result), "WSQ-41: held hidden/unattackable base advances");
                if (!result) return;
                hashes.push_back(result.value().state_sha256);
                if (frame < 2) continue;
                const auto member = session.combat_state(12);
                expect(member && (fogged ? member->attack_target == 0 && !member->direct
                                        : member->attack_target == 20 && member->direct),
                    "WSQ-41: fog clears in this targeting service; weapon refusal retains a visible direct target");
                if (!member) return;
                if (!deadline) deadline = member->next_scan_frame;
                expect(member->next_scan_frame == *deadline,
                    "WSQ-41: neither fog-clear return nor direct retention performs an autonomous scan");
            }
            expect(deadline && *deadline > 5 && *deadline < 65,
                "WSQ-41: a positive prior-scan deadline expires during retained-base checks");
            if (remove_base) expect(static_cast<bool>(session.stage_remove(20)),
                "WSQ-41: removing the hidden base ends repeated adoption");
            else if (reveal_base)
                expect(static_cast<bool>(session.submit({{65, empire, 2}, {}, tactical::RevealAllPayload{empire}})),
                    "WSQ-41: revealing the retained base ends repeated fog clearing");
            else expect(static_cast<bool>(session.submit({{65, empire, 2}, {10}, tactical::StopPayload{}})),
                "FO-03: replacing the base releases the adoption path");
            bool released = false;
            for (int frame = 0; frame < 90; ++frame) {
                const auto result = session.step(executor);
                expect(static_cast<bool>(result), "WSQ-41: base release/reveal advances");
                if (!result) return;
                hashes.push_back(result.value().state_sha256);
                const auto member = session.combat_state(12);
                if (frame >= 2) expect(member && (reveal_base ? member->direct && member->attack_target == 20
                        && deadline && member->next_scan_frame == *deadline : !member->direct),
                    "WSQ-41: reveal retains the visible direct base; removal/replacement permits autonomous targeting");
                released = released || (member && (reveal_base ? member->direct && member->attack_target == 20
                    : !member->direct && member->attack_target == 21 && deadline && member->next_scan_frame > *deadline));
            }
            expect(released, "WSQ-41: an ineligible or revealed base ends the fog-clear loop without resetting the scan clock");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WSQ-41 release hashes match on 1/2/4/8 workers");
        }
    }
}

void test_base_destination_range_entry_and_retention() {
    // WSQ-41: reach admits a new direct target; range loss alone does not
    // release a visible held direct target in the subsequent targeting service.
    auto guns = combat();
    tactical::CombatState held;
    held.attack_target = 20;
    held.direct = true;
    held.next_scan_frame = 7;
    tactical::detail::CombatUnit source;
    source.id = 11; source.type_id = craft_type; source.owner = empire; source.team = 1;
    source.profile = guns.find(craft_type); source.combat = &held;
    tactical::detail::CombatUnit target;
    target.id = 20; target.type_id = frigate_type; target.owner = rebel; target.team = 2;
    target.profile = guns.find(frigate_type);
    target.position = at(2000, 0); target.visible_to = 1;
    const std::array<tactical::SnapshotPlayer, 2> relations{{{empire, 1, false}, {rebel, 2, false}}};
    tactical::detail::CombatWorld world;
    world.relationships = relations; world.table = &guns; world.units = {source, target}; world.frame = 100;
    tactical::detail::FormationAttackContext formation;
    formation.present = true;
    formation.base_attack_target = 20;
    const auto kept = tactical::detail::target_combat(world, world.units.front(), {}, 20, {}, &formation);
    expect(kept.direct && kept.attack_target == 20 && kept.next_scan_frame == 7,
        "WSQ-41: a visible direct holder survives range loss without an autonomous scan");

    held.attack_target = 0;
    held.direct = false;
    target.visible_to = 0;
    auto alternative = source;
    alternative.id = 21; alternative.owner = rebel; alternative.team = 2;
    alternative.position = at(100, 0); alternative.visible_to = 1; alternative.combat = nullptr;
    const auto owners = players();
    world.players = owners;
    world.units = {source, target, alternative};
    const std::array<tactical::SpaceBody, 3> bodies{{{11, empire, source.position},
        {20, rebel, target.position}, {21, rebel, alternative.position}}};
    world.index = tactical::SpaceIndex::build(bodies).value();
    guns.priority_sets.front().rows = {{craft_type, units(1)}, {frigate_type, std::nullopt}};
    const auto found = tactical::detail::target_combat(world, world.units.front(), {}, 20, {}, &formation);
    expect(!found.direct && found.attack_target == 21 && found.next_scan_frame > 100,
        "WSQ-41: an out-of-reach hidden base cannot repeat adoption and block an already-due scan");
}

void test_base_destination_buzz_replacement() {
    constexpr tactical::TypeId droid_type = 500;
    for (int origin = 0; origin < 3; ++origin) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.seed = seed; setup.players = players();
            setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0)),
                unit(12, craft_type, empire, at(-10, 10)), unit(20, frigate_type, rebel, at(450, 0)),
                unit(30, frigate_type, empire, at(400, 0))};
            setup.squadrons = {{10, {11, 12}}};
            auto movement = motion(); movement.squadrons.spawners.clear();
            auto health = durability();
            health.profiles.push_back({squadron_a, units(1000), std::nullopt, false, {}});
            health.profiles.push_back({droid_type, units(20), std::nullopt, false, {}});
            std::sort(health.profiles.begin(), health.profiles.end(), [](const auto& left, const auto& right) {
                return left.type_id < right.type_id;
            });
            tactical::AbilityProfile buzz;
            buzz.kind = tactical::AbilityKind::buzz_droids; buzz.recharge_frames = 60;
            buzz.buzz = tactical::BuzzDroidsProfile{droid_type, units(5), units(400), units(300), units(50),
                units(5), units(1), units(20), 1000, 1, 0};
            tactical::AbilityTable abilities;
            abilities.profiles = {{squadron_a, {buzz}}};
            abilities.humans = {empire, rebel};
            auto made = tactical::TacticalSession::create(setup, sensors(), health, movement,
                std::nullopt, combat(), {}, abilities);
            expect(static_cast<bool>(made), "WSQ-41/FS-23: buzz adoption fixture creates");
            if (!made) return;
            auto session = std::move(made).value();
            const tactical::CommandPayload order = origin == 0 ? tactical::CommandPayload{tactical::AttackPayload{20}}
                : origin == 1 ? tactical::CommandPayload{tactical::AttackMovePayload{{}, 20}}
                : tactical::CommandPayload{tactical::GuardPayload{{}, 30}};
            expect(static_cast<bool>(session.submit({{0, empire, 1}, {10}, order})),
                "WSQ-41/FS-23: ordinary attack/attack-move/friendly guard submits before buzz");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            const auto advance = [&]() {
                const auto result = session.step(executor);
                expect(static_cast<bool>(result), "WSQ-41/FS-23: buzz replacement advances");
                if (result) hashes.push_back(result.value().state_sha256);
                return static_cast<bool>(result);
            };
            for (int frame = 0; frame < 5; ++frame) if (!advance()) return;
            const auto initial = session.combat_state(12);
            expect(initial && initial->attack_target == 20 && initial->direct == (origin != 2),
                "WSQ-41/FS-23: the initial ordinary base determines directness");
            tactical::AbilityPayload request{tactical::AbilityKind::buzz_droids, tactical::AbilityAction::activate};
            request.position = at(600, 0);
            expect(static_cast<bool>(session.submit({{5, empire, 2}, {10}, request})),
                "FS-23: a retained out-of-range buzz request submits after adoption");
            bool pending = false, applied = false;
            for (int frame = 0; frame < 150; ++frame) {
                if (!advance()) return;
                const auto ability = session.ability_state(10);
                const auto member = session.combat_state(12);
                if (frame >= 1 && ability && ability->pending_buzz_position) {
                    pending = true;
                    const auto ordinary = session.squadron_state(10);
                    expect(ordinary && ordinary->target == 20 && ordinary->formation
                        && ordinary->formation->base_target == (origin == 2 ? 30U : 20U)
                        && ordinary->formation->attack_target == 20,
                        "FS-23: pending buzz preserves the ordinary team target and base");
                    expect(member && member->attack_target == 0 && !member->direct,
                        "FT-07: the special approach's plain Move clears member weapon targets while the team holds its base");
                }
                if (ability && ability->slots.front().ready_tick > 0) {
                    applied = true;
                    expect(member && !member->direct,
                        "WSQ-41/FS-23: buzz application Stop permits ordinary autonomous targeting");
                }
            }
            expect(pending && applied, "FS-23: the regression observes both retained approach and application Stop");
            const auto live = session.units();
            expect(std::count_if(live.begin(), live.end(), [](const auto& value) { return value.type_id == droid_type; }) == 1,
                "FS-23: the retained request applies once");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WSQ-41/FS-23: buzz replacement hashes match on 1/2/4/8 workers");
        }
    }
}

} // namespace fighter_test_support
