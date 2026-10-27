#include "eawr/checkpoint/tactical.hpp"
#include "eawr/core/tick_profile.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "land_test_support.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace {
using namespace eawr;
using namespace sim::tactical;
using namespace land_test;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }
template<class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void must(core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }

void independent_motion() {
    auto field = make_field(std::vector<std::string>(20, std::string(40, '.')));
    LandMovement::Checkpoint saved;
    LandMovingSquad team;
    team.object = 100; team.position = at(60, 100); team.generic_team = true;
    // A deliberately unusable wrapper cap must have no effect on LC-83 member motion.
    team.maximum_speed = {};
    saved.squads.push_back(team);
    for (const auto id : {2U, 1U}) {
        LandMovingMember member;
        member.object = id; member.mask = walker; member.position = at(60, 100);
        member.profile = {whole(id == 1 ? 60 : 30), whole(900), whole(900), whole(2), {}, false, id == 1 ? eawr::units::LandCollisionGroup::giant_vehicle : eawr::units::LandCollisionGroup::infantry};
        member.source.locomotor = "synthetic-walking";
        saved.members.push_back(member);
    }
    auto moving = LandMovement::from_checkpoint(saved).value();
    LandPath path; path.status = LandPathStatus::found; path.nodes = {team.position, at(400, 100), at(400, 200)};
    expect(moving.install_path(0, path, 0), "LC-83 path propagates to generic members");
    sim::InlineExecutor executor;
    const auto dt = Fixed::from_ratio(1, 30).value();
    for (int tick = 0; tick < 60; ++tick) must(moving.step(field.occupancy, executor, dt));
    expect(moving.members()[1].position.x.raw() - moving.members()[0].position.x.raw() > 40 * Fixed::scale,
        "members use their own speeds instead of a wrapper displacement or the slowest member cap");
    expect(moving.squads()[0].position == moving.members()[1].position
        && moving.squads()[0].facing == moving.members()[1].facing,
        "wrapper follows lowest member ID despite reversed member storage");
    const auto stopped = moving.checkpoint();
    expect(moving.drop_order(0), "generic stop dispatch accepts the team");
    for (int tick = 0; tick < 30; ++tick) must(moving.step(field.occupancy, executor, dt));
    for (std::size_t i = 0; i < moving.members().size(); ++i)
        expect(moving.members()[i].position == stopped.members[i].position && moving.members()[i].speed == Fixed{}
            && moving.members()[i].state == LandWalkState::stopped, "stop reaches every member locomotor");
    path.nodes.front() = moving.squads()[0].position;
    expect(moving.install_path(0, path, 0), "generic team can receive a replacement order");
    for (int tick = 0; tick < 700; ++tick) must(moving.step(field.occupancy, executor, dt));
    expect(moving.squads()[0].order == LandMoveOrder::reached, "all independent trackers finish a multi-segment route");
    for (const auto& m : moving.members()) expect(m.position == at(400, 200) && m.speed == Fixed{},
        "both different-speed members arrive and stop");
    saved = moving.checkpoint(); saved.members[0].next_node = path.nodes.size() + 1;
    expect(!LandMovement::from_checkpoint(saved), "checkpoint rejects a member tracker outside the shared path");

    // Start admission combines member gates while retaining each member's starting state.
    saved = stopped;
    for (auto& m : saved.members) { m.state = LandWalkState::stopped; m.speed = {}; m.profile.walk_transition = true; }
    saved.members[0].gates.start_almost_finished = true;
    moving = LandMovement::from_checkpoint(saved).value();
    must(moving.step(field.occupancy, executor, dt));
    expect(moving.members()[0].position == saved.members[0].position
        && moving.members()[1].position == saved.members[1].position, "one blocked start gate holds the propagated start");
    auto gates = saved.members[1].gates; gates.start_almost_finished = true;
    expect(moving.set_gates(1, gates), "last member start gate opens");
    must(moving.step(field.occupancy, executor, dt));
    expect(moving.members()[0].position != saved.members[0].position
        && moving.members()[1].position != saved.members[1].position, "all eligible members start after the combined gate opens");
}

TacticalContent content(const Field& field, bool generic, std::size_t count = 3) {
    TacticalContent result;
    result.domain = BattleDomain::land; result.land_terrain = field.terrain; result.land.clear_bit = clear;
    result.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), whole(1)};
    LandCompanyBinding binding;
    binding.type = 11; binding.company.id = "synthetic-company";
    binding.company.max_squad_size = static_cast<std::uint32_t>(count);
    binding.company.team_locomotor = generic ? "TEAM_LOCOMOTOR" : "LAND_TEAM_CONTAINER_LOCOMOTOR";
    if (!generic) binding.effective_team_max_speed = whole(60);
    for (std::size_t i = 0; i < count; ++i) {
        units::LandCompanyMember member;
        member.type = "synthetic-member"; member.kind = units::LandObjectKind::infantry;
        member.create_team = true; member.movement_class = "walker"; member.locomotor = "synthetic-walking";
        binding.company.members.push_back(member);
        LandMemberBinding physical;
        physical.type = 12;
        physical.locomotion = {whole(i % 2 ? 40 : 80), whole(120), whole(150), whole(2), whole(1), false, eawr::units::LandCollisionGroup::bike};
        physical.hard_x = whole(1); physical.hard_y = whole(2);
        binding.members.push_back(physical);
    }
    result.land.companies.push_back(std::move(binding));
    return result;
}
TacticalSetup setup(std::size_t count = 4) {
    TacticalSetup result; result.domain = BattleDomain::land; result.content_identity[0] = 79;
    result.players = {{1, 1, 1, player_flag_commandable}, {2, 2, 2, player_flag_commandable}};
    for (std::size_t i = 0; i < count; ++i) {
        UnitState unit; unit.entity_id = static_cast<sim::EntityId>(i + 1); unit.type_id = 11;
        unit.owner = i < count / 2 ? 1U : 2U;
        unit.position = {whole(60 + static_cast<std::int64_t>(i % 10) * 180),
            whole(60 + static_cast<std::int64_t>(i / 10) * 240), {}};
        result.units.push_back(unit);
    }
    return result;
}
std::vector<std::uint8_t> save(const TacticalSession& session) {
    return take(checkpoint::save_tactical_checkpoint(session, session.pending_commands(),
        checkpoint::default_seats(session.record().setup), checkpoint::tactical_identities(session.record().setup.content_identity)));
}
std::vector<std::uint8_t> session_run(const TacticalSetup& initial, const TacticalContent& tables,
    const sim::PartitionExecutor& executor, bool restore) {
    auto session = take(TacticalSession::create(initial, tables));
    for (const auto tick : {0U, 75U, 90U}) for (const auto& unit : initial.units) {
        if (tick == 75) must(session.submit({{tick, unit.owner, unit.entity_id + 100}, {unit.entity_id}, StopPayload{}}));
        else must(session.submit({{tick, unit.owner, unit.entity_id + (tick == 0 ? 0U : 200U)}, {unit.entity_id},
            MovePayload{{whole(1200), unit.position.y, {}}}}));
    }
    for (int tick = 0; tick < 300; ++tick) {
        expect(take(session.step(executor)).diagnostics.empty(), "generic session accepts path/start/stop commands");
        if (restore && (tick == 12 || tick == 75 || tick == 149)) {
            const auto bytes = save(session);
            auto resumed = take(checkpoint::load_tactical_checkpoint(bytes, checkpoint::tactical_identities(initial.content_identity),
                [&](const TacticalReplay& replay) { return TacticalSession::from_replay(replay, tables); }, executor));
            expect(save(resumed.session) == bytes && resumed.work.resimulated_ticks == 0,
                "section 18 v3 restores independent member trackers without replaying");
            session = std::move(resumed.session);
        }
    }
    auto playback = take(TacticalSession::from_replay(take(parse_replay(take(write_replay(session.record())))), tables));
    for (int tick = 0; tick < 300; ++tick) take(playback.step(executor));
    expect(save(playback) == save(session), "generic-team replay reproduces the entire checkpoint");
    expect(session.land_movement()->squads().front().position.x > initial.units.front().position.x,
        "generic session members physically move");
    return save(session);
}
void session_contracts() {
    auto field = make_field(std::vector<std::string>(128, std::string(128, '.')));
    auto tables = content(field, true);
    const auto initial = setup();
    sim::InlineExecutor serial;
    const auto baseline = session_run(initial, tables, serial, false);
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        expect(session_run(initial, tables, executor, true) == baseline, "generic session bytes equal on 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(workers);
        expect(session_run(initial, tables, reverse, true) == baseline, "generic session bytes equal with reverse partitions");
    }
}
void stress() {
    auto field = make_field(std::vector<std::string>(128, std::string(128, '.')));
    const auto initial = setup(80);
    for (bool generic : {false, true}) {
        const auto tables = content(field, generic, 10);
        std::vector<std::uint8_t> baseline;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto session = take(TacticalSession::create(initial, tables));
            for (const auto& unit : initial.units) must(session.submit({{0, unit.owner, unit.entity_id}, {unit.entity_id},
                MovePayload{{whole(2200), unit.position.y, {}}}}));
            platform::ThreadWorkerAdapter executor(workers);
            core::tick_profile::Recording timing;
            const auto begin = std::chrono::steady_clock::now();
            {
                core::tick_profile::Capture capture(timing);
                for (int tick = 0; tick < 24; ++tick) take(session.step(executor));
            }
            std::cout << "land generic stress objects=880 generic=" << generic << " workers=" << workers
                << " ms/tick=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / 24;
            for (const auto name : {"land-path-slices", "land-member-proposals", "land-contacts", "land-occupancy-commit", "land-publication"}) {
                double total = 0;
                for (const auto& row : timing.costs) if (row.name == name) total += row.ms;
                std::cout << ' ' << name << '=' << total / 24;
            }
            std::cout << '\n';
            expect(session.snapshot()->instances().size() == 880, "stress contains eighty teams and eight hundred members");
            const auto bytes = save(session);
            if (baseline.empty()) baseline = bytes;
            expect(bytes == baseline, "880-object stress is deterministic across workers");
        }
    }
}
}
int main() {
    try { independent_motion(); session_contracts(); stress(); return checks.finish(); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
