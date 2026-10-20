#include "fighter_support.hpp"
#include "fighter_scans_fixtures.hpp"
#include "../../src/sim/tactical/checkpoint_fields_internal.hpp"
#include "../../src/sim/tactical/session_services.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fighter_test_support {

void test_priority_scan() {
    test_base_destination_adoption();
    test_base_destination_lifecycle();
    test_base_destination_release();
    test_base_destination_range_entry_and_retention();
    test_base_destination_buzz_replacement();
    test_launch_completion();
    test_lost_launch_combat_completion();
    test_attack_order_target_loss_hold();
    test_player_escort_completion();
    namespace checkpoint = tactical::checkpoint_detail;
    // WMV-18: a deleted object base becomes a position, so later ordinary
    // acquisition can still install pursuit. An explicitly absent formation
    // remains absent and contributes no allowance.
    for (const int origin : {0, 1, 2, 3, 4, 5, 6}) {
        // Sole-base done service also establishes history after a never-completed
        // mobile launch loses its base and has no live unfinished path.
        for (const int history : {0, 1, 2}) {
            if (origin == 6 && history != 2) continue; // unfinished member-path control
            const bool completed = history == 1;
            const bool reached_done = history != 2;
            std::vector<std::string> reference;
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                tactical::TacticalSetup setup;
                setup.seed = seed;
                setup.players = players();
                setup.units = {unit(10, squadron_a, empire, at(0, 0)),
                    unit(11, craft_type, empire, at(0, 0)), unit(12, craft_type, empire, at(-10, 10)),
                    unit(30, frigate_type, rebel, at(650, 0))};
                setup.squadrons = {{10, {11, 12}}};
                auto table = motion();
                table.squadrons.spawners.clear();
                auto guns = combat();
                auto health = durability();
                if (origin == 5) {
                    // Accepted real weapon damage supplies an attacker; no scripted
                    // DamagePayload and no member scan may drive this acquisition.
                    auto& attacker = guns.profiles[2];
                    attacker.priority_set = 0;
                    attacker.max_attack_distance = units(1000);
                    auto weapon = guns.profiles[1].weapons.front();
                    weapon.range = units(1000);
                    weapon.cone_width = units(720);
                    weapon.cone_height = units(720);
                    weapon.shot = tactical::ShotProfile{units(1), 0, units(1000), units(2000), true, true, {}};
                    attacker.weapons = {weapon};
                    guns.unit_ai_types = {squadron_a};
                    guns.profiles[1].collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
                    tactical::DamageRules rules;
                    rules.shield_recharge_frames = 90;
                    rules.energy_recharge_frames = 150;
                    rules.energy_to_shield = units(5);
                    rules.damage_types = 1;
                    rules.armor_types = 1;
                    rules.armor_mods = {units(1)};
                    rules.diminishing = {{units(0), units(1)}, {units(1), units(1)}, {units(2), units(1)}, {units(3), units(1)}};
                    health.damage = rules;
                }
                auto made = tactical::TacticalSession::create(setup, sensors(), health, table, std::nullopt, guns);
                expect(static_cast<bool>(made), "WMV-18: lost-base fixture builds");
                if (!made) return;
                auto session = std::move(made).value();
                if (origin == 5) expect(static_cast<bool>(session.submit({{0, rebel, 1}, {30}, tactical::AttackPayload{10}})),
                    "WMV-18/WTA-29: attacker receives an ordinary attack order");
                const auto stage = [&](const bool arm) {
                    const auto saved = session.save_checkpoint_state();
                    expect(static_cast<bool>(saved), "WMV-18: staging checkpoint saves");
                    if (!saved) return;
                    checkpoint::Access::State staged(setup, sensors(), health, table, std::nullopt, guns, {}, {}, {}, {});
                    checkpoint::Reader input(saved.value());
                    std::uint32_t version{};
                    input(version);
                    checkpoint::Access::state(input, staged);
                    std::vector<tactical::session_detail::LiveUnit> live;
                    input(live);
                    const auto tail = input.remaining();
                    auto& team = staged.minds.at(10);
                    if (!arm) {
                        if (origin == 3) tactical::session_detail::apply_squadron_order(team,
                            tactical::GuardPayload{at(0, 0), 20}, at(0, 0), table.squadrons);
                        team.target = 20; // destination removed before this service
                        team.formation = tactical::SquadronFormationState{at(-500, -500), 20,
                            completed, reached_done, false, 20, 0};
                        if (origin == 0) team.formation.reset();
                        if (origin == 2 || origin == 4 || origin == 6) {
                            tactical::SquadronAttackPath path;
                            path.target = origin == 4 ? 30U : 20U;
                            tactical::SquadronAttackRoute route;
                            route.nodes = {at(0, 0), origin == 2 ? at(100, 50)
                                : origin == 6 ? at(6000, 0) : at(1200, 100)};
                            if (origin == 6) route.trackers = {{10, {0, true}}, {11, {0, true}}, {12, {0, true}}};
                            path.stack.push_back(std::move(route));
                            team.attack_path = std::move(path);
                        }
                    }
                    for (auto& member : live) if (member.combat
                        && (member.state.entity_id == 11 || member.state.entity_id == 12)) {
                        member.combat->attack_target = arm ? 0U : 20U;
                        member.combat->direct = false;
                        member.combat->next_scan_frame = arm && origin != 5 ? 0U : 1000U;
                    }
                    if (!arm && origin == 5) for (auto& member : live)
                        if (member.state.entity_id == 30 && member.combat) member.combat->weapons.front().countdown = 8;
                    checkpoint::Writer output;
                    output(version);
                    checkpoint::Access::state(output, std::as_const(staged));
                    output(live);
                    output.bytes.insert(output.bytes.end(), saved.value().end() - static_cast<std::ptrdiff_t>(tail), saved.value().end());
                    auto fresh = tactical::TacticalSession::from_replay(session.record_through_next_tick(),
                        sensors(), health, table, std::nullopt, guns);
                    expect(static_cast<bool>(fresh), "WMV-18: staging preserves the sealed command log");
                    if (!fresh) return;
                    const auto loaded = fresh.value().load_checkpoint_state(output.bytes);
                    expect(static_cast<bool>(loaded), loaded ? "WMV-18: staged input restores"
                        : "WMV-18: staged input restores: " + loaded.error().message);
                    if (!loaded) return;
                    session = std::move(fresh).value();
                };
                stage(false);
                const eawr::platform::ThreadWorkerAdapter executor(workers);
                const auto loss = session.step(executor);
                expect(static_cast<bool>(loss), "WMV-18: lost-base service advances");
                if (!loss) return;
                const auto converted = session.squadron_state(10);
                if (origin == 0) expect(converted && !converted->formation,
                    "WMV-18: target loss does not invent an absent formation");
                else {
                    expect(converted && converted->formation && converted->formation->base_target == 0
                        && converted->formation->complete == (origin != 6)
                        && converted->formation->has_reached_done == (origin != 6),
                        "WMV-18: sole-base done completes conversion only without an unfinished member path");
                    expect(converted && converted->formation && converted->formation->base_position
                        == (origin == 2 ? at(100, 50) : origin == 6 ? at(6000, 0) : at(0, 0)),
                        "WMV-18: only a route for the lost base supplies its end; otherwise use the team centre");
                    expect(converted && converted->formation && converted->formation->attack_target == 0
                        && converted->formation->attack_hardpoint == tactical::attack_hull,
                        "WMV-18: ordinary loss clears the stale attack attachment");
                }
                stage(true);
                std::vector<std::string> hashes{loss.value().state_sha256};
                bool acquired = false, pursued = false, fired = false, damaged = false;
                std::optional<tactical::TacticalSession> continued;
                for (int tick = 1; tick < 200; ++tick) {
                    if (tick == 60) {
                        auto copy = tactical::TacticalSession::from_replay(session.record(), sensors(), health, table, std::nullopt, guns);
                        const auto saved = session.save_checkpoint_state();
                        expect(copy && saved, "WMV-18: continuation fixture and checkpoint exist");
                        if (!copy || !saved) return;
                        continued.emplace(std::move(copy).value());
                        const auto restored = continued->load_checkpoint_state(saved.value());
                        expect(static_cast<bool>(restored), restored ? "WMV-18: pursuit checkpoint restores"
                            : "WMV-18: pursuit checkpoint restores: " + restored.error().message);
                        if (!restored) return;
                        expect(continued->canonical_state_bytes() == session.canonical_state_bytes(),
                            "WMV-18: restored pursuit and command cursors have identical canonical state");
                    }
                    const auto step = session.step(executor);
                    expect(static_cast<bool>(step), "WMV-18: recovered acquisition advances");
                    if (!step) return;
                    hashes.push_back(step.value().state_sha256);
                    if (continued) {
                        const auto resumed = continued->step(executor);
                        expect(resumed && resumed.value().state_sha256 == step.value().state_sha256,
                            "WMV-18: pursuit continuation matches every uninterrupted hash");
                    }
                    const auto mind = session.squadron_state(10);
                    acquired = acquired || (mind && mind->target == 30);
                    pursued = pursued || (mind && mind->formation && mind->formation->attack_target == 30);
                    for (const auto& event : step.value().snapshot->combat_events()) {
                        fired = fired || (event.kind == tactical::CombatEventKind::weapon_fired
                            && (event.shooter == 11 || event.shooter == 12));
                        damaged = damaged || (event.kind == tactical::CombatEventKind::projectile_hit && event.shooter == 30
                            && (event.target == 11 || event.target == 12));
                    }
                }
                if (origin == 5) expect(damaged, "WMV-18/WTA-29: actual attacker damage reaches a member");
                if (origin == 0) expect(!acquired && !pursued,
                    "WMV-18: an absent formation does not acquire beyond own attack reach");
                else if (origin == 6) expect(!acquired && !pursued,
                    "WMV-18: an unfinished member path withholds completion and idle pursuit");
                else expect(acquired && pursued && fired,
                    "WMV-18: lost attack/escort base recovers accepted pursuit and firing");
                if (workers == 1) reference = hashes;
                else expect(hashes == reference, "WMV-18: loss and recovery hashes match on 1/2/4/8 workers");
            }
        }
    }
    // Player-controlled reserves beside an ongoing engagement. Distances are
    // fixture inputs; acquisition still uses authored FT-02/FO-06 allowances.
    for (const int origin : {0, 1, 2, 3, 4}) {
        for (const int distance : {100, 650, 1050, 1400}) {
            std::vector<std::string> reference;
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                const bool launched = origin >= 3;
                tactical::TacticalSetup setup;
                setup.seed = seed;
                setup.players = players();
                setup.units = {unit(1, carrier_type, empire, at(0, 0)),
                    unit(20, frigate_type, rebel, at(distance, 0)),
                    unit(30, squadron_b, empire, at(distance, 100)),
                    unit(31, craft_type, empire, at(distance, 100))};
                setup.squadrons = {{30, {31}}};
                if (!launched) {
                    const auto start = origin == 1 ? at(-1500, 0) : at(0, 0);
                    setup.units.push_back(unit(10, squadron_a, empire, start));
                    setup.units.push_back(unit(11, craft_type, empire, start));
                    setup.units.push_back(unit(12, craft_type, empire,
                        {Fixed::from_raw(start.x.raw() - 10 * one), units(10), {}}));
                    setup.squadrons.insert(setup.squadrons.begin(), {10, {11, 12}});
                }
                std::sort(setup.units.begin(), setup.units.end(),
                    [](const auto& left, const auto& right) { return left.entity_id < right.entity_id; });
                auto table = motion();
                if (launched) {
                    table.squadrons.spawners.front().entries = {{squadron_a, 1, 0}};
                    table.squadrons.spawners.front().mobile = origin == 4;
                } else table.squadrons.spawners.clear();
                auto made = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
                expect(static_cast<bool>(made), "player reserve: the engagement fixture creates");
                if (!made) return;
                auto session = std::move(made).value();
                expect(static_cast<bool>(session.submit({{0, empire, 1}, {30}, tactical::AttackPayload{20}})),
                    "player reserve: the nearby allied fighter is ordered to fight");
                if (origin == 1) expect(static_cast<bool>(session.submit({{0, empire, 2}, {10},
                    tactical::MovePayload{at(0, 0)}})), "player reserve: the plain move submits");
                if (origin == 2) expect(static_cast<bool>(session.submit({{0, empire, 2}, {10},
                    tactical::GuardPayload{at(0, 0), 0}})), "player reserve: guard of the point submits");
                const eawr::platform::ThreadWorkerAdapter executor(workers);
                std::vector<std::string> hashes;
                std::optional<std::uint64_t> acquired, pursuing, fired;
                eawr::sim::EntityId reserve = launched ? 0U : 10U;
                bool allied_fire = false;
                for (int frame = 0; frame < 600; ++frame) {
                    const auto step = session.step(executor);
                    expect(static_cast<bool>(step), "player reserve: the engagement advances");
                    if (!step) return;
                    hashes.push_back(step.value().state_sha256);
                    if (reserve == 0) for (const auto& team : session.squadrons())
                        if (team.container != 30) { reserve = team.container; break; }
                    const auto mind = session.squadron_state(reserve);
                    if (mind && mind->target == 20 && !acquired) acquired = step.value().completed_tick;
                    if (mind && mind->formation && mind->formation->attack_target == 20 && !pursuing)
                        pursuing = step.value().completed_tick;
                    for (const auto& event : step.value().snapshot->combat_events()) {
                        if (event.kind != tactical::CombatEventKind::weapon_fired) continue;
                        allied_fire = allied_fire || event.shooter == 31;
                        if (mind && std::find(mind->roster.begin(), mind->roster.end(), event.shooter) != mind->roster.end()
                            && !fired) fired = step.value().completed_tick;
                    }
                }
                expect(allied_fire, "player reserve: the neighbouring engagement actually fires");
                // Authored fixture reach 500 plus idle allowance 200 = 700;
                // guard/mobile escort allowance 1000 plus reach 500 = 1500.
                const bool in_range = distance <= ((origin == 2 || origin == 4) ? 1500 : 700);
                if (in_range) {
                    expect(acquired && pursuing && fired,
                        "player reserve: an in-range idle/guard/completed-move/launched squadron acquires, pursues and fires");
                } else {
                    expect(!pursuing, "player reserve: a fight beyond the formation allowance is not pursued");
                }
                if (workers == 1) {
                    reference = hashes;
                    std::cout << "player reserve origin=" << origin << " distance=" << distance
                        << " acquire=" << acquired.value_or(9999) << " pursue=" << pursuing.value_or(9999)
                        << " fire=" << fired.value_or(9999) << '\n';
                } else expect(hashes == reference, "player reserve: all tick hashes match on 1/2/4/8 workers");
            }
        }
    }
    test_serial_attack_point_retarget();
    test_member_service_prefix();
    test_team_target_loss();
    test_same_target_share_cell();
    test_direct_attack_approach();
    const auto run_case = [](std::vector<tactical::UnitState> enemies, const bool shaped = false, const bool custom = false) {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(1, carrier_type, empire, at(-3000, 0)), unit(10, squadron_a, empire, at(0, 0)),
            unit(11, craft_type, empire, at(0, 0)), unit(12, craft_type, empire, at(-10, 10))};
        for (auto& enemy : enemies) setup.units.push_back(enemy);
        setup.squadrons = {{10, {11, 12}}};
        auto table = motion();
        table.squadrons.spawners.clear();
        if (custom) table.footprints = {{frigate_type, tactical::SpaceLayer::frigate, units(5), units(5), units(5)}};
        auto guns = combat();
        if (shaped) for (auto& profile : guns.profiles) {
            if (profile.type_id == frigate_type) profile.collision = tactical::CollisionBox{at(-80, -5, -5), at(80, 5, 5)};
        }
        auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, guns);
        expect(static_cast<bool>(created), "C-05: the session builds");
        std::optional<tactical::CombatState> state;
        if (!created) return state;
        auto session = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter executor(1);
        for (int tick = 0; tick < 5; ++tick) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "C-05: steps");
            if (!stepped) return state;
        }
        state = session.combat_state(12);
        return state;
    };
    const auto chosen = run_case({unit(20, frigate_type, rebel, at(650, 0)), unit(21, corvette_type, rebel, at(300, 0))});
    expect(chosen && chosen->attack_target == 20 && !chosen->direct, "C-05: the priority-1 frigate is an autonomous target");
    expect(chosen && chosen->next_scan_frame >= 30 && chosen->next_scan_frame <= 45,
        "WSQ-43: a priority-one share preserves the receiving member's scan clock");
    const auto only_near = run_case({unit(21, corvette_type, rebel, at(300, 0))});
    expect(only_near && only_near->attack_target == 21 && !only_near->direct,
        "C-05 (WSQ-41): an in-range autonomous override over an idle base remains autonomous");
    // Idle chase 200 + attack distance 500 = 700 from the held point.
    const auto beyond = run_case({unit(20, frigate_type, rebel, at(900, 0))});
    expect(beyond && !beyond->direct, "C-05: a unit beyond the chase range plus attack distance is not the squadron's");

    auto long_target = unit(20, frigate_type, rebel, at(650, 300));
    const auto end_on = run_case({long_target}, true);
    expect(end_on && end_on->attack_target == 20 && !end_on->direct,
        "WSQ-45/AT-10: the target's long facing extent admits it beyond the centre-only diversion reach");
    const auto narrowed = run_case({long_target}, true, true);
    expect(narrowed && narrowed->attack_target == eawr::sim::invalid_entity_id,
        "WSQ-45/AV-05: authored hard extents override the target's longer collision bounds");
    const auto quarter_turn = math::normalize(math::Quat{Fixed{}, Fixed{}, units(1), units(1)});
    expect(static_cast<bool>(quarter_turn), "WSQ-45/AT-10: the target's quarter turn builds");
    if (quarter_turn) long_target.rotation = quarter_turn.value();
    const auto broadside = run_case({long_target}, true);
    expect(broadside && broadside->attack_target == eawr::sim::invalid_entity_id,
        "WSQ-45/AT-10: a target seen across its short extent remains beyond diversion reach");

    // FSC-75/UFS-35: a child can be suitable while its normalized space-team
    // parent is outside the allowance-circle reach. Large authored roster slots
    // must not supply the specialized formation-team attack geometry.
    std::string parent_extent_hash;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0)),
            unit(12, craft_type, empire, at(600, 0)), unit(20, squadron_b, rebel, at(750, 0)),
            unit(21, craft_type, rebel, at(705, 0)), unit(22, craft_type, rebel, at(795, 0))};
        setup.squadrons = {{10, {11, 12}}, {20, {21, 22}}};
        auto table = motion();
        table.squadrons.spawners.clear();
        for (auto& profile : table.squadrons.squadrons) {
            if (profile.type_id != squadron_b) continue;
            profile.members = {craft_type, craft_type};
            profile.offsets = {at(-100, 0), at(100, 0)};
        }
        auto guns = combat();
        guns.footprints = {{craft_type, units(30), units(30), units(30)},
            {squadron_a, {}, {}, {}}, {squadron_b, {}, {}, {}}};
        std::sort(guns.footprints.begin(), guns.footprints.end(),
            [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
        auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, guns);
        expect(static_cast<bool>(created), "FSC-75: space-parent extent control builds");
        if (!created) continue;
        auto session = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto step = session.step(executor);
        expect(static_cast<bool>(step), "FSC-75: space-parent extent control steps");
        if (!step) continue;
        const auto mind = session.squadron_state(10);
        const auto leader = session.combat_state(11), scanner = session.combat_state(12);
        expect(mind && mind->target == 20 && mind->formation && mind->formation->attack_target == 0,
            "FSC-75: suitable child shares the parent without admitting its roster extent");
        expect(leader && scanner && leader->attack_target == 0 && scanner->attack_target == 20,
            "FSC-75: the normalized parent is kept only by a member in its own firing reach");
        if (workers == 1) parent_extent_hash = step.value().state_sha256;
        else expect(step.value().state_sha256 == parent_extent_hash,
            "FSC-75: parent extent refusal is equal on 1/2/4/8 workers");
    }

    // WSQ-42/43/47/20: only the follower can find the enemy. Its scan is shared as the
    // enemy team and immediately notifies the whole roster, even before reaching the cell.
    // FSC-75: refusal still shares/pairs, while accepted movement retention and
    // the scanner's independent deadline remain distinct from path installation.
    for (const bool admitted : {false, true}) {
        const bool adopted = admitted;
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.seed = seed;
            setup.players = players();
            const int leader_x = admitted ? 200 : 0;
            const int follower_x = admitted ? -200 : -650;
            const int enemy_x = admitted ? -650 : -1100;
            setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(leader_x, 0)),
                unit(12, craft_type, empire, at(follower_x, 0)), unit(20, squadron_b, rebel, at(enemy_x, 0)),
                unit(21, craft_type, rebel, at(enemy_x, 0)), unit(22, craft_type, rebel, at(enemy_x, 10))};
            setup.squadrons = {{10, {11, 12}}, {20, {21, 22}}};
            auto table = motion();
            table.squadrons.spawners.clear();
            auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
            expect(static_cast<bool>(created), "WSQ-42: follower acquisition fixture builds");
            if (!created) continue;
            auto session = std::move(created).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            std::uint64_t scanner_deadline{};
            for (int tick = 0; tick < 5; ++tick) {
                const auto stepped = session.step(executor);
                expect(static_cast<bool>(stepped), "WSQ-42: follower acquisition steps");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                const auto mind = session.squadron_state(10);
                const auto scanner_now = session.combat_state(12);
                if (tick == 0 && scanner_now) scanner_deadline = scanner_now->next_scan_frame;
                expect(scanner_now && scanner_now->next_scan_frame == scanner_deadline,
                    "FSC-75: notifications/adoption/path work do not shorten the finding member's scan clock");
                if (tick == 0) expect(mind && (adopted ? mind->attack_path.has_value() : !mind->attack_path.has_value()),
                    "FSC-75: only the accepted formation destination can request the later attack path");
                expect(mind && mind->target == 20 && mind->formation && mind->formation->has_reached_done
                    && mind->formation->attack_override == adopted
                    && mind->formation->attack_target == (adopted ? 20U : 0U),
                    "WDF-03/WMV-18: team sharing and completion history survive either diversion result");
                if (tick == 0 && adopted) {
                    expect(mind && mind->formation && !mind->formation->complete
                        && mind->formation->base_position == at(0, 0),
                        "WMV-18: a completed position base is reset to the split team's current position");
                } else {
                    expect(mind && mind->formation && mind->formation->complete,
                        "WMV-18: a space-team attack override with completed-base history initializes as done");
                }
                for (const eawr::sim::EntityId id : {11U, 12U}) {
                    const auto target = session.combat_state(id);
                    expect(target && target->attack_target == (adopted || id == 12 ? 20U : 0U)
                        && (tick != 0 || !target->direct),
                        "WSQ-43/47/WDF-02: share the normalized squadron, then retain each member within its own reach");
                }
                if (tick == 0) {
                    const auto leader = session.craft_state(11), follower = session.craft_state(12);
                    const auto chased1 = session.craft_state(21), chased2 = session.craft_state(22);
                    // FS-18/20: the later-created opposing roster scans first.
                    // Its reservation prevents the older group pairing back.
                    expect(leader && follower && chased1 && chased2 && leader->chase == eawr::sim::invalid_entity_id
                            && follower->chase == eawr::sim::invalid_entity_id
                            && leader->chase_until == 301 && follower->chase_until == 301
                            && chased1->chase == 11 && chased2->chase == 12
                            && chased1->chase_until == 301 && chased2->chase_until == 301,
                        "FS-18/WSQ-20: first serviced acquisition pairs in roster order and reserves both clocks");
                    const auto scanner = session.combat_state(12);
                    expect(scanner && scanner->next_scan_frame >= 30 && scanner->next_scan_frame <= 45,
                        "WSQ-42: the member owns its 30+0..15-frame scan clock");
                }
            }
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WSQ-42/43/20: acquisition hashes match on 1/2/4/8 workers");
            expect(static_cast<bool>(session.submit({{6, empire, 1}, {10}, tactical::MovePayload{at(2000, 0)}})),
                "FT-07: move after autonomous acquisition submits");
            for (int tick = 0; tick < 3; ++tick) {
                const auto stepped = session.step(executor);
                expect(static_cast<bool>(stepped), "FT-07: move after autonomous acquisition steps");
            }
            for (const eawr::sim::EntityId id : {11U, 12U}) {
                const auto state = session.combat_state(id);
                expect(state && state->attack_target == eawr::sim::invalid_entity_id && !state->direct,
                    "FT-07: removing the formation target clears every member's held target");
            }
        }
    }
}

} // namespace fighter_test_support
