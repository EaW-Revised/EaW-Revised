#include "fighter_support.hpp"
#include "../../src/sim/tactical/fighters_internal.hpp"
#include "../../src/sim/tactical/checkpoint_fields_internal.hpp"
#include "../../src/sim/tactical/session_services.hpp"

namespace fighter_test_support {

namespace {

void test_member_service_prefix() {
    // FSC-75/FD-69: supplied scan outcomes and clocks, not a guessed period.
    auto guns = combat();
    tactical::detail::CombatWorld world;
    world.table = &guns;
    const auto decimal = [](const char* value) { return Fixed::from_decimal(value).value(); };
    tactical::detail::CombatUnit leader;
    leader.id = 1;
    leader.position = at(400, -1500);
    leader.profile = guns.find(craft_type);
    tactical::detail::CombatUnit parent;
    parent.id = 20;
    parent.position = {decimal("-303.156799"), decimal("-1358.170288"), {}};
    parent.transform = math::to_matrix(math::identity_quat(), parent.position).value();
    auto child = parent;
    child.id = 21;
    world.units = {leader, parent, child};
    world.teams = {{20, {21}}};
    tactical::SquadronState team;
    team.container = 10;
    team.formation = tactical::SquadronFormationState{at(400, -1500), 0, true, true, false, 0};
    std::array<tactical::CombatState, 7> members;
    const std::array<std::uint64_t, 7> initial{460, 447, 478, 461, 458, 445, 459};
    std::vector<tactical::CombatState*> roster;
    for (std::size_t slot = 0; slot < members.size(); ++slot) {
        members[slot].next_scan_frame = initial[slot];
        roster.push_back(&members[slot]);
    }
    const tactical::detail::FormationAttackContext context{true, true, false, &world.units.front()};
    const auto visit = [&](const std::uint64_t supplied, const std::size_t scanner,
                           const std::uint64_t deadline, const std::vector<std::size_t>& held) {
        std::array<tactical::CombatState, 7> weapon_inputs;
        for (std::size_t turn = members.size(); turn-- > 0;) {
            auto output = members[turn];
            if (turn == scanner) {
                output.attack_target = 21;
                output.next_scan_frame = deadline;
            }
            world.frame = supplied + 1;
            auto committed = tactical::detail::commit_squadron_target(world, world.units.front(),
                output, members[turn], roster, team, at(400, -1500), units(200), at(400, -1500), context, false);
            expect(static_cast<bool>(committed), "FSC-75: each staged member has one targeting tail");
            weapon_inputs[turn] = members[turn];
        }
        for (std::size_t slot = 0; slot < members.size(); ++slot) {
            const bool retained = std::find(held.begin(), held.end(), slot + 1) != held.end();
            expect(members[slot].attack_target == (retained ? 20U : 0U),
                "FSC-75: supplied 101/102/103/104/114 held-member table matches service rank");
            if (slot != scanner) expect(members[slot].next_scan_frame == initial[slot]
                || (slot == 5 && members[slot].next_scan_frame == 489)
                || (slot == 1 && members[slot].next_scan_frame == 492),
                "FC-41/73: shares and clears preserve independent clocks");
        }
        if (scanner < members.size()) {
            expect(members[scanner].next_scan_frame == deadline,
                "FC-73: only the scanner consumes its supplied synchronization deadline");
            for (std::size_t prefix = scanner + 1; prefix < members.size(); ++prefix)
                expect(weapon_inputs[prefix].attack_target == 0,
                    "FD-69: prefix weapon inputs precede the incoming share");
        }
    };
    visit(445, 5, 489, {7});
    expect(team.formation->attack_target == 0, "FSC-75: first parent notification refuses");
    visit(446, 7, 0, {});
    world.units[1].position = {decimal("-294.429016"), decimal("-1356.072876"), {}};
    visit(447, 1, 492, {3, 4, 5, 6, 7});
    expect(team.formation->attack_target == 0, "FSC-75: second parent notification refuses");
    visit(448, 7, 0, {});
    world.units[1].position = {decimal("-241.303802"), decimal("-1355.927490"), {}};
    visit(458, 4, 495, {1, 2, 3, 4, 5, 6, 7});
    expect(team.formation->attack_target == 20 && !team.attack_path,
        "FSC-73/75: accepted destination retains the suffix before path installation");
    expect(members[5].next_scan_frame - 445 == 44 && members[1].next_scan_frame - 447 == 45
        && members[4].next_scan_frame - 458 == 37, "FC-73: independent 44/45/37-frame intervals survive sharing");
}

// WSQ-44 (debug build, EWSQ-26): a member that loses the team's held target (fog, stealth or limbo
// clears its own target in ship targeting) shares the loss. The team, every member still holding
// it and the formation destination let go; a target the team does not hold is left alone.
void test_team_target_loss() {
    auto guns = combat();
    tactical::detail::CombatWorld world;
    world.table = &guns;
    tactical::detail::CombatUnit leader;
    leader.id = 1;
    leader.position = at(0, 0);
    leader.profile = guns.find(craft_type);
    tactical::detail::CombatUnit target;
    target.id = 20;
    target.position = at(400, 0);
    world.units = {leader, target};
    const tactical::detail::FormationAttackContext context{true, true, false, &world.units.front()};
    const auto lose = [&](const eawr::sim::EntityId held, tactical::SquadronState& team,
                          std::array<tactical::CombatState, 3>& members) {
        team.container = 10;
        team.target = held;
        team.target_hardpoint = 4;
        team.approach = true;
        team.formation = tactical::SquadronFormationState{at(0, 0), 0, false, true, true, 20};
        for (auto& member : members) {
            member.attack_target = 20;
            member.attack_hardpoint = 4;
            member.direct = true;
        }
        members[2].attack_target = 30; // a craft with a target of its own
        std::vector<tactical::CombatState*> roster{&members[0], &members[1], &members[2]};
        auto scanned = members[1];
        scanned.attack_target = eawr::sim::invalid_entity_id;
        scanned.attack_hardpoint = tactical::no_hardpoint;
        scanned.direct = false;
        auto committed = tactical::detail::commit_squadron_target(world, world.units.front(), scanned,
            members[1], roster, team, at(0, 0), units(200), at(0, 0), context, false);
        expect(static_cast<bool>(committed), "WSQ-44: the loss commits");
        return committed ? committed.value() : tactical::detail::SquadronTargetCommit{};
    };
    {
        tactical::SquadronState team;
        std::array<tactical::CombatState, 3> members;
        const auto result = lose(20, team, members);
        expect(result.lost && !result.shared, "WSQ-44: losing the held target is a loss, not a share");
        expect(team.target == eawr::sim::invalid_entity_id && team.target_hardpoint == tactical::attack_hull
            && !team.approach, "WSQ-44: the team drops its target, hardpoint and approach");
        expect(members[0].attack_target == eawr::sim::invalid_entity_id
            && members[0].attack_hardpoint == tactical::no_hardpoint && !members[0].direct
            && members[1].attack_target == eawr::sim::invalid_entity_id,
            "WSQ-44: every member holding the target lets go");
        expect(members[2].attack_target == 30, "WSQ-44: a member's different target is kept");
        expect(team.formation && team.formation->attack_target == eawr::sim::invalid_entity_id
            && !team.formation->attack_override && team.formation->complete == team.formation->has_reached_done,
            "WSQ-44: the formation destination is cleared and its override undone");
    }
    {
        tactical::SquadronState team;
        std::array<tactical::CombatState, 3> members;
        const auto result = lose(30, team, members);
        expect(!result.lost && team.target == 30 && team.approach && team.formation
            && team.formation->attack_target == 20,
            "WSQ-44: a member's loss of a target the team does not hold leaves the team alone");
    }
}

// WSQ-43/49: sharing from a member with no target leaves the team's combat
// cell even when the normalized find is already the team's target.
void test_same_target_share_cell() {
    namespace checkpoint = tactical::checkpoint_detail;
    for (const bool targetless : {false, true}) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.seed = seed;
            setup.players = players();
            setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0)),
                unit(12, craft_type, empire, at(-10, 10)), unit(20, squadron_b, rebel, at(100, 0)),
                unit(21, craft_type, rebel, at(100, 0))};
            setup.squadrons = {{10, {11, 12}}, {20, {21}}};
            auto table = motion();
            table.squadrons.spawners.clear();
            auto guns = combat();
            guns.priority_sets.front().rows.insert(guns.priority_sets.front().rows.begin(), {craft_type, units(1)});
            auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, guns);
            expect(static_cast<bool>(created), created ? "WSQ-43/49: same-target share session builds"
                : "WSQ-43/49: same-target share session builds: " + created.error().message);
            if (!created) return;
            auto session = std::move(created).value();
            const auto saved = session.save_checkpoint_state();
            expect(static_cast<bool>(saved), "WSQ-43/49: initial checkpoint exists");
            if (!saved) return;
            // Stage the service input through the checkpoint field archive, without
            // exposing mutable session state or relying on random scan timing.
            checkpoint::Access::State staged(setup, sensors(), durability(), table, std::nullopt, guns, {}, {}, {}, {});
            checkpoint::Reader input(saved.value());
            std::uint32_t version{};
            input(version);
            checkpoint::Access::state(input, staged);
            std::vector<tactical::session_detail::LiveUnit> live;
            input(live);
            const auto tail = input.remaining();
            const tactical::CombatCell cell{0, 0};
            for (const auto id : {10U, 20U}) {
                auto& team = staged.minds.at(id);
                team.target = id == 10 ? 20U : 10U;
                team.cell = cell;
                team.joined = true;
                team.approach = false;
                team.formation->attack_target = team.target;
                team.next_scan_frame = 0;
            }
            for (auto& member : live) {
                if (!member.combat) continue;
                const auto id = member.state.entity_id;
                if (id != 11 && id != 12 && id != 21) continue;
                member.combat->attack_target = id == 21 ? 10U : (id == 12 && targetless ? 0U : 20U);
                member.combat->next_scan_frame = id == 12 ? 0U : 1000U;
            }
            checkpoint::Writer output;
            output(version);
            checkpoint::Access::state(output, std::as_const(staged));
            output(live);
            output.bytes.insert(output.bytes.end(), saved.value().end() - static_cast<std::ptrdiff_t>(tail), saved.value().end());
            expect(static_cast<bool>(session.load_checkpoint_state(output.bytes)), "WSQ-43/49: staged checkpoint loads");
            const auto before = session.squadron_state(10);
            expect(before && before->target == 20 && before->cell == cell && before->joined,
                "WSQ-43/49: team holds the target in a joined combat cell before the scan");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (int tick = 0; tick < 5; ++tick) {
                const auto stepped = session.step(executor);
                expect(static_cast<bool>(stepped), "WSQ-43/49: same-target share steps");
                if (!stepped) return;
                hashes.push_back(stepped.value().state_sha256);
                if (tick != 0) continue;
                const auto team = session.squadron_state(10);
                const auto scanner = session.combat_state(12);
                expect(team && team->target == 20 && scanner && scanner->attack_target == 20,
                    "WSQ-43: the team and scanning member keep the same normalized target");
                expect(team && (targetless ? (!team->cell && !team->joined) : (team->cell == cell && team->joined)),
                    "WSQ-43/49: a targetless member's share leaves combat; an already-held find keeps the cell");
            }
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WSQ-43/49: same-target share hashes match on 1/2/4/8 workers");
        }
    }
}

// FA-07: a player's attack starts the approach when it changes the formation's attack destination,
// as an autonomous find does, but never splits the team's base destination (WMV-18).
void test_direct_attack_approach() {
    auto guns = combat();
    tactical::detail::CombatWorld world;
    world.table = &guns;
    tactical::detail::CombatUnit leader;
    leader.id = 1;
    leader.position = at(0, 0);
    leader.profile = guns.find(craft_type);
    tactical::detail::CombatUnit target;
    target.id = 20;
    target.position = at(400, 0);
    world.units = {leader, target};
    const tactical::detail::FormationAttackContext context{true, true, false, &world.units.front()};
    tactical::SquadronState team;
    team.container = 10;
    team.formation = tactical::SquadronFormationState{at(0, 0), 0, true, true, false, 0};
    tactical::CombatState member;
    std::array<tactical::CombatState, 1> roster_states{};
    std::vector<tactical::CombatState*> roster{&roster_states[0]};
    auto scanned = member;
    scanned.attack_target = 20;
    scanned.direct = true;
    auto committed = tactical::detail::commit_squadron_target(world, world.units.front(), scanned, member,
        roster, team, at(0, 0), units(200), at(0, 0), context, false);
    expect(static_cast<bool>(committed) && committed.value().admitted && committed.value().shared,
        "FA-07: a player's attack is admitted and shared");
    expect(team.approach && team.formation && team.formation->attack_target == 20,
        "FA-07: a direct attack on a new destination starts the approach");
    expect(team.formation && !team.formation->attack_override && team.formation->complete
        && team.formation->base_position == at(0, 0),
        "WMV-18: a direct attack leaves the team's base destination alone");
}

} // namespace

void test_serial_attack_point_retarget() {
    namespace checkpoint = tactical::checkpoint_detail;
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        std::vector<std::string> diverted;
        for (const bool already_resolved : {false, true}) {
            tactical::TacticalSetup setup;
            setup.seed = seed;
            setup.players = players();
            setup.units = {unit(10, squadron_b, empire, at(200, 200)), unit(11, craft_type, empire, at(200, 200)),
                unit(20, carrier_type, empire, at(2500, 0)), unit(30, squadron_b, rebel, at(210, 200)),
                unit(31, craft_type, rebel, at(210, 200))};
            setup.squadrons = {{10, {11}}, {30, {31}}};
            auto table = motion();
            table.squadrons.spawners.clear();
            auto health = durability();
            tactical::HardpointProfile hardpoint;
            hardpoint.destroyable = true;
            hardpoint.max_health = units(100);
            health.profiles.front().hardpoints = {hardpoint};
            auto guns = combat();
            guns.profiles.front().hardpoints = {{0, at(0, 2000), true}};
            auto created = tactical::TacticalSession::create(setup, sensors(), health, table, std::nullopt, guns);
            expect(static_cast<bool>(created), created ? "serial retarget: session creates"
                : "serial retarget: session creates: " + created.error().message);
            if (!created) return;
            auto session = std::move(created).value();
            const auto saved = session.save_checkpoint_state();
            if (!saved) { expect(false, "serial retarget: initial checkpoint exists"); return; }
            checkpoint::Access::State staged(setup, sensors(), health, table, std::nullopt, guns, {}, {}, {}, {});
            checkpoint::Reader input(saved.value());
            std::uint32_t version{};
            input(version);
            checkpoint::Access::state(input, staged);
            std::vector<tactical::session_detail::LiveUnit> live;
            input(live);
            const auto tail = input.remaining();
            auto& attacker = staged.minds.at(10);
            attacker.target = 30;
            attacker.cell = tactical::CombatCell{0, 0};
            attacker.joined = true;
            attacker.next_scan_frame = 1000;
            attacker.formation->attack_target = 30;
            auto& later = staged.minds.at(30);
            later.target = 0;
            later.cell = attacker.cell;
            later.joined = true;
            later.next_scan_frame = 1000;
            later.formation->base_target = 20;
            later.formation->complete = false;
            later.formation->has_reached_done = false;
            later.formation->attack_override = already_resolved;
            later.formation->attack_target = already_resolved ? 10U : 20U;
            later.formation->attack_hardpoint = already_resolved ? tactical::attack_hull : 0U;
            for (auto& member : live) if (member.combat) {
                member.combat->next_scan_frame = 1000;
                member.combat->attack_target = member.state.entity_id == 11 ? 30U : 0U;
            }
            checkpoint::Writer output;
            output(version);
            checkpoint::Access::state(output, std::as_const(staged));
            output(live);
            output.bytes.insert(output.bytes.end(), saved.value().end() - static_cast<std::ptrdiff_t>(tail), saved.value().end());
            expect(static_cast<bool>(session.load_checkpoint_state(output.bytes)), "serial retarget: staged input restores");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (int frame = 0; frame < 4; ++frame) {
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), step ? "serial retarget: FD-04 turn succeeds"
                    : "serial retarget: FD-04 turn succeeds: " + step.error().message);
                if (!step) return;
                hashes.push_back(step.value().state_sha256);
                const auto mind = session.squadron_state(30);
                expect(mind && mind->formation && mind->formation->attack_target == 10
                        && mind->formation->attack_hardpoint == tactical::attack_hull,
                    "serial retarget: the later team's new squadron target cannot retain the old station attachment");
            }
            if (!already_resolved) diverted = hashes;
            else expect(hashes == diverted,
                "serial retarget: motion and full state match a control already resolved against the new destination");
        }
        if (workers == 1) reference = diverted;
        else expect(diverted == reference, "serial retarget: staged diversion is equal on 1/2/4/8 workers");
    }
}

namespace {

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

} // namespace

void test_priority_scan() {
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
// WDF-05: both grids laid over the map box (Coruscant's low corner, -6500 on both axes).
void test_map_box_grids() {
    const tactical::MapBox box{units(-6500), units(-6500)};
    // The S-97 recording: the X-wing squadron flew to the cell point (500, -1500), which only the
    // box-anchored grid has.
    const auto recorded = tactical::combat_cell_of(at(500, -1500), box);
    expect(recorded == tactical::CombatCell{17, 12}, "WDF-05: (500, -1500) lies in cell (17, 12) of the box grid");
    const auto recorded_point = tactical::combat_cell_point(recorded, units(9), box);
    expect(recorded_point.x == units(500) && recorded_point.y == units(-1500) && recorded_point.z == units(9),
        "WDF-05: cell (17, 12)'s point is the recorded (500, -1500) at the craft's height");
    expect(tactical::combat_cell_point(tactical::combat_cell_of(at(500, -1500), tactical::MapBox{}), Fixed{}, tactical::MapBox{}).x
            != units(500),
        "WDF-05: the world-origin grid has no point at (500, -1500)");
    // An odd row moves x back half a cell and its point half a cell forward.
    const auto odd = tactical::combat_cell_of(at(700, -1100), box);
    expect(odd == tactical::CombatCell{17, 13}, "WDF-05: an odd row shifts x half a cell");
    const auto odd_point = tactical::combat_cell_point(odd, Fixed{}, box);
    expect(odd_point.x == units(700) && odd_point.y == units(-1100), "WDF-05: an odd row's cell point");
    // x is clamped to the box's low x after the odd-row shift.
    expect(tactical::combat_cell_of(at(-6450, -6000), box) == tactical::CombatCell{0, 1},
        "WDF-05: an odd row's x clamps to the box's low x");
    const auto corner = tactical::combat_cell_point({0, 1}, Fixed{}, box);
    expect(corner.x == units(-6100) && corner.y == units(-5900), "WDF-05: cell (0, 1)'s point");
    expect(tactical::combat_cell_of(at(-6500, -6500), box) == tactical::CombatCell{0, 0}, "WDF-05: the low corner is cell (0, 0)");
    // Negative offsets truncate toward zero: a point under the box by less than a cell is row 0.
    expect(tactical::combat_cell_of(at(-6500, -6700), box) == tactical::CombatCell{0, 0},
        "WDF-05: the row truncates, so 200 units under the box is row 0");
    expect(tactical::combat_cell_of(at(-6500, -7000), box) == tactical::CombatCell{0, -1},
        "WDF-05: 500 units under the box is row -1, and its odd row clamps x");
    expect(tactical::combat_cell_of(at(-7000, -6400), box) == tactical::CombatCell{0, 0}, "WDF-05: x left of the box clamps to column 0");
    // The idle grid is the same rule at 120 units.
    expect(tactical::idle_cell_of(at(-6400, -6400), box) == tactical::CombatCell{0, 0}, "WDF-05: idle cell (0, 0)");
    const auto idle_point = tactical::idle_cell_point({0, 0}, units(4), box);
    expect(idle_point.x == units(-6440) && idle_point.y == units(-6440) && idle_point.z == units(4),
        "WDF-05: idle cell (0, 0)'s point is min + 60");
    expect(tactical::idle_cell_of(at(-6300, -6300), box) == tactical::CombatCell{1, 1}, "WDF-05: an odd idle row shifts x half a cell");
    const auto idle_odd = tactical::idle_cell_point({1, 1}, Fixed{}, box);
    expect(idle_odd.x == units(-6260) && idle_odd.y == units(-6320), "WDF-05: an odd idle row's point");
    // The cell point round-trips inside the box, on both grids.
    for (std::int32_t y = 0; y < 8; ++y) {
        for (std::int32_t x = 0; x < 8; ++x) {
            const tactical::CombatCell cell{x, y};
            expect(tactical::combat_cell_of(tactical::combat_cell_point(cell, Fixed{}, box), box) == cell,
                "WDF-05: a combat cell's point lies in that cell");
            expect(tactical::idle_cell_of(tactical::idle_cell_point(cell, Fixed{}, box), box) == cell,
                "WDF-05: an idle cell's point lies in that cell");
        }
    }
    // The claim searches around the cell under the point, in the box's grid.
    const auto claimed = tactical::idle_cell_claim(at(-6300, -6300), {}, box);
    expect(claimed && *claimed == tactical::CombatCell{1, 1}, "WDF-05: the idle claim starts at the box grid's cell");
}

// C-30 (#687, FM-23, FM-24): the idle cell a squadron claims.
void test_idle_cell_claim() {
    const auto desired = at(70, 250);
    const auto start = tactical::idle_cell_of(desired, tactical::MapBox{});
    expect(start.x == 0 && start.y == 2, "FM-23: (70, 250) lies in cell (0, 2)");
    const auto point = tactical::idle_cell_point(start, units(20), tactical::MapBox{});
    expect(point.x == units(60) && point.y == units(300) && point.z == units(20), "FM-23: cell (0, 2)'s point is (60, 300)");
    const auto odd = tactical::idle_cell_point({0, 1}, Fixed{}, tactical::MapBox{});
    expect(odd.x == units(120) && odd.y == units(180), "FM-23: an odd row is shifted half a cell");
    const auto free = tactical::idle_cell_claim(desired, {}, tactical::MapBox{});
    expect(free && *free == start, "FM-24: the free cell under the desired point");
    const std::vector<tactical::CombatCell> taken{start};
    const auto next = tactical::idle_cell_claim(desired, taken, tactical::MapBox{});
    expect(next && *next == tactical::CombatCell{0, 1}, "FM-24: the nearest free cell of the first ring, (120, 180)");
    std::vector<tactical::CombatCell> full;
    for (std::int32_t y = start.y - 4; y <= start.y + 4; ++y) {
        for (std::int32_t x = start.x - 4; x <= start.x + 4; ++x) full.push_back({x, y});
    }
    expect(!tactical::idle_cell_claim(desired, full, tactical::MapBox{}), "FM-24: no claim once 64 cells show no free one");
    full.erase(std::find(full.begin(), full.end(), tactical::CombatCell{start.x + 3, start.y + 3}));
    expect(tactical::idle_cell_claim(desired, full, tactical::MapBox{}).has_value(),
        "FM-24: a free cell of the third ring is found within the 64 examined");
}

// C-31 (#687, FM-24 to FM-26): squadrons 10 (craft 11, 12) and 20 (craft 21) each moved to (1500,
// 0) by their own order hold two idle cells; the mobile carrier 1's two launched squadrons escort it and
// hold two more. The run records the hashes and the cells at the end.
struct IdleRun {
    std::vector<std::string> hashes;
    std::vector<std::pair<eawr::sim::EntityId, tactical::CombatCell>> cells; // ascending container
};

// WDF-05: the idle fixture lies below the world origin; its box starts below the carrier.
const tactical::MapBox idle_box{units(-4800), units(-4800)};

[[nodiscard]] IdleRun run_idle(const std::size_t workers) {
    IdleRun run;
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(1, carrier_type, empire, at(-2000, -2000)), unit(10, squadron_a, empire, at(0, 0)),
        unit(11, craft_type, empire, at(0, 0)), unit(12, craft_type, empire, at(-10, 10)),
        unit(20, squadron_b, empire, at(0, 300)), unit(21, craft_type, empire, at(0, 300))};
    setup.squadrons = {{10, {11, 12}}, {20, {21}}};
    auto table = motion();
    table.squadrons.spawners[0].mobile = true; // a carrier: its launches escort it (FL-07)
    auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat(),
        tactical::VictoryRules{}, tactical::AbilityTable{}, tactical::EconomyRules{}, idle_box);
    expect(static_cast<bool>(created), "C-31: the session builds");
    if (!created) return run;
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.submit({{5, empire, 1}, {10}, tactical::MovePayload{at(1500, 0)}})), "C-31: move 10");
    expect(static_cast<bool>(session.submit({{5, empire, 2}, {20}, tactical::MovePayload{at(1500, 0)}})), "C-31: move 20");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < 900; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "C-31: step failed: " + stepped.error().message);
            return run;
        }
        run.hashes.push_back(stepped.value().state_sha256);
    }
    for (eawr::sim::EntityId id = 1; id < 200; ++id) {
        if (const auto state = session.squadron_state(id); state && state->idle_cell) run.cells.emplace_back(id, *state->idle_cell);
    }
    return run;
}

void test_idle_grid() {
    const auto run = run_idle(1);
    expect(run.hashes.size() == 900, "C-31: 900 ticks");
    const auto held = [&run](const eawr::sim::EntityId id) -> std::optional<tactical::CombatCell> {
        for (const auto& [container, cell] : run.cells) {
            if (container == id) return cell;
        }
        return std::nullopt;
    };
    expect(held(10) && held(20) && !(*held(10) == *held(20)), "FM-24: two squadrons moved to one point hold two cells");
    std::size_t escorts = 0;
    for (const auto& [container, cell] : run.cells) {
        if (container != 10 && container != 20) ++escorts;
    }
    expect(escorts == 2, "FM-24: the carrier's two escorts each hold a cell, got " + std::to_string(escorts));
    for (std::size_t a = 0; a < run.cells.size(); ++a) {
        for (std::size_t b = a + 1; b < run.cells.size(); ++b) {
            const auto p = tactical::idle_cell_point(run.cells[a].second, Fixed{}, idle_box);
            const auto q = tactical::idle_cell_point(run.cells[b].second, Fixed{}, idle_box);
            const double dx = static_cast<double>(p.x.raw() - q.x.raw()) / static_cast<double>(one);
            const double dy = static_cast<double>(p.y.raw() - q.y.raw()) / static_cast<double>(one);
            expect(std::sqrt(dx * dx + dy * dy) >= 119.0, "FM-23: held cells are at least a cell apart");
        }
    }
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run_idle(workers).hashes == run.hashes, "C-31: " + std::to_string(workers) + " workers hash like one");
    }
}
// fires at the squadron's craft nearest itself. A gunship (30) attacks squadron 20 (craft 21, 22),
// and squadron 10 (craft 11, 12) attacks it from beyond its chase reach; 1, 2, 4 and 8 workers
// hash alike.
constexpr tactical::TypeId gunship_type = 420; // an object weapon, no motion

struct TeamRun {
    std::vector<std::string> hashes;
    std::vector<tactical::CombatEvent> fired; // weapon_fired events of 30, 11 and 12
    std::optional<tactical::CombatState> gunship;
};

[[nodiscard]] TeamRun run_team_attack(const std::size_t workers) {
    TeamRun run;
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(10, squadron_a, empire, at(-1500, 0)), unit(11, craft_type, empire, at(-1500, 0)),
        unit(12, craft_type, empire, at(-1510, 10)), unit(20, squadron_a, rebel, at(300, 0)),
        unit(21, craft_type, rebel, at(300, 0)), unit(22, craft_type, rebel, at(290, 10)),
        unit(30, gunship_type, empire, at(0, 0))};
    setup.squadrons = {{10, {11, 12}}, {20, {21, 22}}};
    auto table = motion();
    table.squadrons.spawners.clear();
    auto health = durability();
    health.profiles.push_back(tactical::DurabilityProfile{gunship_type, units(5000), std::nullopt, false, {}});
    auto weapons = combat();
    auto gunship = weapons.profiles[1]; // the craft's gun and attack distance
    gunship.type_id = gunship_type;
    gunship.category_bits = 4;
    gunship.priority_set.reset();
    weapons.profiles.push_back(gunship);
    auto sight = sensors();
    sight.push_back({gunship_type, units(8000)});
    auto created = tactical::TacticalSession::create(setup, sight, health, table, std::nullopt, weapons);
    expect(static_cast<bool>(created), "C-10: the session builds");
    if (!created) return run;
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.submit({{1, empire, 1}, {30}, tactical::AttackPayload{20}})), "C-10: the gunship's attack submits");
    expect(static_cast<bool>(session.submit({{1, empire, 2}, {10}, tactical::AttackPayload{20}})), "C-10: the squadron's attack submits");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < 600; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "C-10: step failed: " + stepped.error().message);
            break;
        }
        run.hashes.push_back(stepped.value().state_sha256);
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            if (event.kind == tactical::CombatEventKind::weapon_fired
                && (event.shooter == 30 || event.shooter == 11 || event.shooter == 12)) {
                run.fired.push_back(event);
            }
        }
        if (tick == 3) run.gunship = session.combat_state(30);
    }
    return run;
}

void test_team_attack() {
    const auto run = run_team_attack(1);
    expect(run.hashes.size() == 600, "C-10: 600 ticks");
    expect(run.gunship && run.gunship->attack_target == 20 && run.gunship->direct,
        "C-10: the gunship's target is the squadron's team container");
    const auto fired_by = [&run](const std::initializer_list<eawr::sim::EntityId> shooters) {
        std::vector<tactical::CombatEvent> events;
        for (const auto& event : run.fired) {
            if (std::find(shooters.begin(), shooters.end(), event.shooter) != shooters.end()) events.push_back(event);
        }
        return events;
    };
    const auto gunship = fired_by({30});
    expect(!gunship.empty(), "C-10: the gunship fires at the squadron it was ordered to attack");
    // Craft 22 (290, 10) is nearer the gunship at the origin than craft 21 (300, 0).
    expect(!gunship.empty() && gunship.front().target == 22, "C-10: the first shot goes to the nearest craft");
    const auto squadron = fired_by({11, 12});
    expect(!squadron.empty(), "C-10: the ordered squadron fires at the enemy squadron");
    for (const auto& event : run.fired) {
        expect(event.target == 21 || event.target == 22,
            "C-10: every shot of an attacker goes to a craft of the squadron, got " + std::to_string(event.target));
    }
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run_team_attack(workers).hashes == run.hashes, "C-10: " + std::to_string(workers) + " workers hash like one");
    }
}

// FM-20 (#1192): a placed squadron's idle hold is its leader's own point, not the container's.
// The leader starts on it, so it no longer aims back past the container and the wingman's
// half-distance speed no longer runs away; 1, 2, 4 and 8 workers alike. `tracks` takes crafts 11
// and 12 at every frame, the start included.
[[nodiscard]] std::vector<std::string> run_placed_idle(const std::size_t workers, std::optional<math::Vec3>* anchor,
    std::array<std::vector<math::Vec3>, 2>* tracks) {
    std::vector<std::string> hashes;
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(10, squadron_a, empire, at(0, 0)), unit(11, craft_type, empire, at(20, -30)),
        unit(12, craft_type, empire, at(-10, 30))};
    setup.squadrons = {{10, {11, 12}}};
    auto table = motion();
    table.squadrons.spawners.clear();
    auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
    expect(static_cast<bool>(created), "FM-20: the session builds");
    if (!created) return hashes;
    auto session = std::move(created).value();
    if (anchor != nullptr) {
        const auto state = session.squadron_state(10);
        if (state) *anchor = state->anchor;
    }
    if (tracks != nullptr) {
        (*tracks)[0].push_back(at(20, -30));
        (*tracks)[1].push_back(at(-10, 30));
    }
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < 60; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "FM-20: step failed: " + stepped.error().message);
            break;
        }
        hashes.push_back(stepped.value().state_sha256);
        if (tracks != nullptr) {
            for (const auto& unit : session.units()) {
                if (unit.entity_id == 11) (*tracks)[0].push_back(unit.position);
                if (unit.entity_id == 12) (*tracks)[1].push_back(unit.position);
            }
        }
    }
    return hashes;
}

void test_placed_idle_hold() {
    // A valid rotation can have no planar heading. Initialization must not
    // dereference the failed yaw result; zero is the deterministic project fallback.
    {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(10, squadron_b, empire, at(0, 0)), unit(11, craft_type, empire, at(0, 0))};
        const auto half = Fixed::from_raw(one / 2);
        setup.units[1].rotation = {half, half, Fixed::from_raw(-one / 2), half};
        expect(!tactical::yaw_degrees(setup.units[1].rotation), "vertical craft rotation has no planar yaw");
        setup.squadrons = {{10, {11}}};
        auto table = motion();
        table.squadrons.spawners.clear();
        auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table);
        expect(static_cast<bool>(created), "placed vertical craft initializes without an unchecked yaw read");
        if (created) {
            const auto flight = created.value().craft_state(11);
            expect(flight && flight->yaw == Fixed{}, "placed vertical craft uses the deterministic zero heading");
        }
    }
    std::optional<math::Vec3> anchor;
    std::array<std::vector<math::Vec3>, 2> tracks;
    const auto hashes = run_placed_idle(1, &anchor, &tracks);
    expect(anchor && anchor->x == units(20) && anchor->y == units(-30),
        "FM-20: a placed squadron holds its leader's point, not the container's");
    expect(tracks[0].size() == 61 && tracks[1].size() == 61, "FM-20: both craft are tracked over 60 frames");
    // Test-only bound, no runtime clamp (G-F10): retail's idle craft peak at about 2.6 times Max_Speed
    // in the S-97 capture (377 and 372 units/s against the craft's own maximum), so no idle craft of
    // a placed squadron may exceed three times its type's Max_Speed in any frame. Before the leader
    // point hold the wingmen climbed to four to six times in half a second.
    const auto profiles = motion(); // the table outlives the pointer into it
    const auto* craft = profiles.squadrons.find_craft(craft_type);
    expect(craft != nullptr, "FM-20: the fixture craft profile exists");
    if (craft != nullptr) {
        const double bound = 3.0 * static_cast<double>(craft->max_speed.raw()) / static_cast<double>(one);
        for (const auto& track : tracks) {
            for (std::size_t frame = 1; frame < track.size(); ++frame) {
                const double dx = static_cast<double>(track[frame].x.raw() - track[frame - 1].x.raw()) / static_cast<double>(one);
                const double dy = static_cast<double>(track[frame].y.raw() - track[frame - 1].y.raw()) / static_cast<double>(one);
                const double dz = static_cast<double>(track[frame].z.raw() - track[frame - 1].z.raw()) / static_cast<double>(one);
                expect(std::sqrt(dx * dx + dy * dy + dz * dz) <= bound,
                    "FM-20: an idle craft of a placed squadron stays within 3x Max_Speed, frame "
                    + std::to_string(frame));
            }
        }
    }
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run_placed_idle(workers, nullptr, nullptr) == hashes,
            "FM-20: " + std::to_string(workers) + " workers hash like one");
    }
}

// C-12 (FO-04): the craft an attack on a squadron fires at is not kept. Each shot goes to the
// squadron's live craft nearest the shooter at that frame (only a special ability's own target is
// kept, and M2 has none). The gunship (30) attacks squadron 20 while 20 flies past it to the far
// side, so the nearest craft changes; 1, 2, 4 and 8 workers hash alike.
struct RetargetShot {
    tactical::CombatEvent event;
    math::Vec3 first{};   // craft 21 at the shot's frame
    math::Vec3 second{};  // craft 22 at the shot's frame
};

struct RetargetRun {
    std::vector<std::string> hashes;
    std::vector<RetargetShot> shots;
};

[[nodiscard]] RetargetRun run_team_retarget(const std::size_t workers) {
    RetargetRun run;
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    setup.units = {unit(20, squadron_a, rebel, at(900, 0)), unit(21, craft_type, rebel, at(900, 0)),
        unit(22, craft_type, rebel, at(880, 30)), unit(30, gunship_type, empire, at(0, 0))};
    setup.squadrons = {{20, {21, 22}}};
    auto table = motion();
    table.squadrons.spawners.clear();
    auto health = durability();
    health.profiles.push_back(tactical::DurabilityProfile{gunship_type, units(5000), std::nullopt, false, {}});
    for (auto& profile : health.profiles) {
        if (profile.type_id == craft_type) profile.max_hull = units(100000); // the pass outlives the fire
    }
    auto weapons = combat();
    auto gunship = weapons.profiles[1];
    gunship.type_id = gunship_type;
    gunship.category_bits = 4;
    gunship.priority_set.reset();
    weapons.profiles.push_back(gunship);
    auto sight = sensors();
    sight.push_back({gunship_type, units(8000)});
    auto created = tactical::TacticalSession::create(setup, sight, health, table, std::nullopt, weapons);
    expect(static_cast<bool>(created), "C-12: the session builds");
    if (!created) return run;
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.submit({{1, empire, 1}, {30}, tactical::AttackPayload{20}})), "C-12: the attack submits");
    expect(static_cast<bool>(session.submit({{1, rebel, 1}, {20}, tactical::MovePayload{at(-2500, 0)}})), "C-12: the move submits");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (int tick = 0; tick < 700; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "C-12: step failed: " + stepped.error().message);
            break;
        }
        run.hashes.push_back(stepped.value().state_sha256);
        const auto& snapshot = *stepped.value().snapshot;
        const auto* first = instance(snapshot, 21);
        const auto* second = instance(snapshot, 22);
        for (const auto& event : snapshot.combat_events()) {
            if (event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 30 && first && second) {
                run.shots.push_back({event, position(*first), position(*second)});
            }
        }
    }
    return run;
}

void test_team_retarget() {
    const auto run = run_team_retarget(1);
    expect(run.hashes.size() == 700, "C-12: 700 ticks");
    expect(run.shots.size() > 4, "C-12: the gunship fires through the pass, got " + std::to_string(run.shots.size()));
    const auto squared = [](const math::Vec3& point) {
        const double x = static_cast<double>(point.x.raw()) / static_cast<double>(one);
        const double y = static_cast<double>(point.y.raw()) / static_cast<double>(one);
        const double z = static_cast<double>(point.z.raw()) / static_cast<double>(one);
        return x * x + y * y + z * z;
    };
    bool hit_first = false;
    bool hit_second = false;
    for (const RetargetShot& shot : run.shots) {
        const double first = squared(shot.first);
        const double second = squared(shot.second);
        hit_first = hit_first || shot.event.target == 21;
        hit_second = hit_second || shot.event.target == 22;
        if (std::abs(first - second) < 1.0) continue; // too close to call in doubles
        const eawr::sim::EntityId nearest = first < second ? 21 : 22;
        expect(shot.event.target == nearest, "C-12: the shot at frame " + std::to_string(shot.event.tick)
            + " goes to the nearest craft " + std::to_string(nearest) + ", got " + std::to_string(shot.event.target));
    }
    expect(hit_first && hit_second, "C-12: the gunship moves to the other craft when it becomes the nearer");
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run_team_retarget(workers).hashes == run.hashes, "C-12: " + std::to_string(workers) + " workers hash like one");
    }

    // WSQ-64: the original attack destination dies, but other live targets remain.
    // WMV-18 preserves its converted position formation; a later autonomous
    // replacement must not rebase the already-idle hold point.
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        tactical::TacticalSetup setup;
        setup.seed = seed;
        setup.players = players();
        setup.units = {unit(10, squadron_a, empire, at(1000, 500, -200)),
            unit(11, craft_type, empire, at(1000, 500, -200)),
            unit(12, craft_type, empire, at(990, 510, -200)),
            unit(20, corvette_type, rebel, at(1300, 500, -200)),
            unit(30, corvette_type, rebel, at(1100, 700))};
        setup.squadrons = {{10, {11, 12}}};
        auto table = motion();
        table.squadrons.spawners.clear();
        auto created = tactical::TacticalSession::create(setup, sensors(), durability(), table, std::nullopt, combat());
        expect(static_cast<bool>(created), "WSQ-64: target-loss fixture builds");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(tactical::PlayerCommand{{0, empire, 0}, {10}, tactical::AttackPayload{20}})),
            "WSQ-64: original attack submits");
        expect(static_cast<bool>(session.submit(tactical::PlayerCommand{{8, empire, 1}, {20}, tactical::DamagePayload{units(4000)}})),
            "WSQ-64: original target destruction submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        bool replaced = false;
        for (std::uint64_t tick = 0; tick < 160; ++tick) {
            if (tick == 70) {
                const auto held = session.squadron_state(10);
                expect(held && held->formation && held->formation->base_target == 0 && held->target == 30,
                    "WSQ-64: converted formation retains the other live target before replacement");
                const auto spawned = session.stage_spawn(unit(0, frigate_type, rebel, at(1000, 600)));
                expect(spawned && spawned.value() == 31, "WSQ-64: better live target stages at the next stable ID");
            }
            const auto before = session.squadron_state(10);
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "WSQ-64: target-loss and retarget step completes");
            if (!step) break;
            hashes.push_back(step.value().state_sha256);
            const auto after = session.squadron_state(10);
            if (before && after && before->target == 30 && after->target == 31) {
                replaced = true;
                expect(before->formation && after->formation && after->formation->attack_target == 31,
                    "WSQ-64: autonomous replacement uses the preserved formation");
                expect(after->anchor == before->anchor,
                    "WSQ-64: replacing a live target preserves the idle hold point");
                const auto leader = session.combat_state(11), follower = session.combat_state(12);
                expect(leader && follower && (leader->attack_target == 31 || follower->attack_target == 31),
                    "WSQ-64: replacement still reaches member targeting");
            }
        }
        expect(replaced, "WSQ-64: a live autonomous target replacement was observed");
        expect(hashes.size() == 160, "WSQ-64: the complete target-loss control ran");
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "WSQ-64: target-loss retarget hashes agree on 1/2/4/8 workers");
    }
}


} // namespace fighter_test_support
