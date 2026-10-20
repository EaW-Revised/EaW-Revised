#include "fighter_support.hpp"
#include "fighter_scans_fixtures.hpp"
#include "../../src/sim/tactical/fighters_internal.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"
#include "../../src/sim/tactical/checkpoint_fields_internal.hpp"
#include "../../src/sim/tactical/session_types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fighter_test_support {

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

} // namespace fighter_test_support
