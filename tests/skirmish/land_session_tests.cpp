#include "eawr/checkpoint/tactical.hpp"
#include "eawr/core/tick_profile.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/presentation/live/land_ground.hpp"
#include "eawr/skirmish/land_terrain.hpp"
#include "eawr/vfs/vfs.hpp"
#include "land_test_support.hpp"
#include "../support/env.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace {
using namespace eawr;
using namespace sim::tactical;
using namespace land_test;
using Bytes = std::vector<std::uint8_t>;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }
template <class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void must(core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }

TacticalContent content(std::shared_ptr<const LandTerrain> terrain, std::size_t member_count = 3) {
    TacticalContent result;
    result.domain = BattleDomain::land; result.land_terrain = std::move(terrain);
    result.land.clear_bit = clear;
    result.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), whole(1)};
    LandCompanyBinding binding;
    binding.type = 11; binding.company.id = "synthetic-company";
    binding.company.team_max_speed = whole(60); binding.company.max_squad_size = static_cast<std::uint32_t>(member_count);
    binding.effective_team_max_speed = whole(60);
    binding.formation_spacing = Fixed::from_ratio(1, 5).value();
    for (std::size_t i = 0; i < member_count; ++i) {
        units::LandCompanyMember member;
        member.type = "synthetic-member"; member.kind = units::LandObjectKind::infantry;
        member.create_team = true; member.movement_class = "walker"; member.max_speed = whole(80);
        member.locomotor = "synthetic-walking";
        binding.company.members.push_back(member);
        binding.members.push_back({12, {whole(80), whole(120), whole(150), whole(2), whole(1), false}});
    }
    result.land.companies.push_back(std::move(binding));
    return result;
}

TacticalSetup setup() {
    TacticalSetup result;
    result.domain = BattleDomain::land;
    result.seed = 49;
    result.content_identity[0] = 7;
    result.players = {{1, 1, 1, player_flag_commandable}, {2, 2, 2, player_flag_commandable}};
    for (sim::EntityId id = 1; id <= 4; ++id) {
        UnitState unit;
        unit.entity_id = id; unit.type_id = 11; unit.owner = id <= 2 ? 1U : 2U;
        unit.position = {whole(60), whole(80 + static_cast<std::int64_t>(id) * 100), {}};
        result.units.push_back(unit);
    }
    return result;
}

void commands(TacticalSession& session) {
    for (std::uint32_t player = 1; player <= 2; ++player) {
        const auto first = static_cast<sim::EntityId>((player - 1) * 2 + 1);
        for (sim::EntityId id = first; id < first + 2; ++id)
            must(session.submit({{0, player, id}, {id}, MovePayload{{whole(620), whole(80 + static_cast<std::int64_t>(id) * 100), {}}}}));
        must(session.submit({{75, player, 10}, {first}, StopPayload{}}));
        must(session.submit({{90, player, 11}, {first}, MovePayload{{whole(620), whole(80 + static_cast<std::int64_t>(first) * 100), {}}}}));
    }
}

Bytes checkpoint_bytes(const TacticalSession& session) {
    return take(checkpoint::save_tactical_checkpoint(session, session.pending_commands(),
        checkpoint::default_seats(session.record().setup), checkpoint::tactical_identities(session.record().setup.content_identity)));
}
checkpoint::TacticalSessionFactory factory(const TacticalContent& tables) {
    return [&tables](const TacticalReplay& replay) { return TacticalSession::from_replay(replay, tables); };
}

struct Result { Bytes state, checkpoint; };
Result run(const TacticalSetup& initial, const TacticalContent& tables, const sim::PartitionExecutor& executor,
    bool restore) {
    auto session = take(TacticalSession::create(initial, tables));
    commands(session);
    expect(session.land_movement()->squads().size() == 4 && session.land_movement()->members().size() == 12,
        "two squads per side expand from L2 company plans");
    const auto start = session.land_movement()->squads()[0].position;
    const auto identities = checkpoint::tactical_identities(initial.content_identity);
    for (int tick = 0; tick < 360; ++tick) {
        const auto stepped = take(session.step(executor));
        expect(stepped.diagnostics.empty(), "land moves and stops execute without rejections");
        if (restore && (tick == 0 || tick == 1 || tick == 12 || tick == 74 || tick == 75 || tick == 90 || tick == 210)) {
            const auto saved = checkpoint_bytes(session);
            auto resumed = take(checkpoint::load_tactical_checkpoint(saved, identities, factory(tables), executor));
            expect(resumed.work.resimulated_ticks == 0, "land checkpoint restores directly without replaying elapsed ticks");
            expect(checkpoint_bytes(resumed.session) == saved, "full land checkpoint bytes round trip at a barrier");
            session = std::move(resumed.session);
        }
    }
    expect(session.land_movement()->squads()[0].position != start, "an ordered land squad physically moves");
    auto replay = take(parse_replay(take(write_replay(session.record()))));
    auto playback = take(TacticalSession::from_replay(replay, tables));
    for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) take(playback.step(executor));
    expect(playback.canonical_state_bytes() == session.canonical_state_bytes(), "land replay reproduces full movement/contact/path state");
    expect(checkpoint_bytes(playback) == checkpoint_bytes(session), "land replay reproduces checkpoint bytes");
    return {session.canonical_state_bytes(), checkpoint_bytes(session)};
}

void live_contracts() {
    auto field = make_field(std::vector<std::string>(40, std::string(48, '.')));
    const auto tables = content(field.terrain);
    const auto initial = setup();
    platform::LiveSession::Options options;
    options.workers = 2;
    options.pacing = platform::LiveSession::Pacing::driven;
    auto live = take(platform::LiveSession::start(initial, tables, options));
    expect(live->land_roster().size() == 12, "live adapter retains expanded land member membership");
    for (const auto& [member, root] : live->land_roster())
        expect(member != root && root >= 1 && root <= 4, "live member binds to its actual commandable root");
    must(live->submit({1, {1}, MovePayload{{whole(620), whole(180), {}}}, 0}));
    must(live->submit({1, {1}, StopPayload{}, 75}));
    live->advance_to(90);
    expect(live->wait_for(90, std::chrono::seconds(30)), "live land move/stop reaches driven tick");
    live->stop();
    expect(!live->failure() && live->rejected_orders().empty(), "live land commands are admitted");
    const auto recording = live->record();
    const auto hashes = take(platform::headless_tick_hashes(recording, tables));
    expect(hashes == live->tick_hashes(), "viewer feed land hashes equal headless replay");
    auto playback = take(platform::LiveSession::start(recording, tables, options));
    playback->advance_to(90);
    expect(playback->wait_for(90, std::chrono::seconds(30)), "land live replay retains full content");
    playback->stop();
    expect(playback->tick_hashes() == hashes, "land live replay is identical");
    const auto ground = presentation::live::land_ground_point(*field.terrain,
        {{60, 180, 1000}, {0, 0, -1}});
    expect(ground && std::abs((*ground)[2]) < 0.01F, "vertical land pick reaches the L1 field");
    expect(!presentation::live::land_ground_point(*field.terrain, {{-1, 180, 1000}, {0, 0, -1}}),
        "land pick outside field has no plane fallback");
    expect(!presentation::live::land_ground_point(*field.terrain, {{60, 180, 1000}, {0, 0, 1}}),
        "land pick behind the camera is rejected");
    sim::tactical::LandTerrainInput sloped;
    sloped.width = 3; sloped.height = 2; sloped.spacing = whole(10);
    sloped.heights = {whole(0), whole(10), whole(0), whole(0), whole(10), whole(0)};
    sloped.surfaces.resize(6); sloped.passability.resize(6, 1);
    const auto ridge = take(sim::tactical::LandTerrain::build(std::move(sloped)));
    const auto hit = presentation::live::land_ground_point(ridge, {{0, 5, 5}, {1, 0, 0}});
    expect(hit && std::abs((*hit)[0] - 5) < 0.01F && std::abs((*hit)[2] - 5) < 0.01F,
        "shallow land pick finds the nearest ridge instead of the space plane");
    const auto back = presentation::live::land_ground_point(ridge, {{20, 5, 5}, {-1, 0, 0}});
    expect(back && std::abs((*back)[0] - 15) < 0.01F, "negative land ray traverses grid boundaries");
}

void contracts() {
    auto field = make_field(std::vector<std::string>(40, std::string(48, '.')));
    auto tables = content(field.terrain);
    auto initial = setup();
    UnitState structure; structure.entity_id = 5; structure.type_id = 13; structure.owner = 1;
    structure.position = {whole(300), whole(180), whole(17)};
    initial.units.push_back(structure);
    tables.land.static_objects.push_back(structure.type_id);
    LandOccupation building; building.object = structure.entity_id; building.primary_passability = wall;
    for (std::uint32_t y = 7; y <= 11; ++y) for (std::uint32_t x = 14; x <= 16; ++x)
        building.primary_cells.push_back(y * field.columns + x);
    building.primary_cells.push_back(building.primary_cells.front());
    std::reverse(building.primary_cells.begin(), building.primary_cells.end());
    tables.land.occupations.push_back(building);
    expect(!TacticalSession::create(initial), "the content-free land seam still refuses creation");
    auto absent = tables; absent.land_terrain.reset();
    const auto refused = TacticalSession::create(initial, absent);
    expect(!refused && refused.error().code == diagnostic_codes::land_provider_unavailable,
        "land terrain absence retains the provider diagnostic");
    const sim::InlineExecutor serial;
    auto unordered = initial;
    std::swap(unordered.units[0], unordered.units[1]);
    expect(!TacticalSession::create(unordered, tables), "land roots require ascending identities before member allocation");
    auto session = take(TacticalSession::create(initial, tables));
    expect(session.unit_state(5) == structure, "static root preserves identity, owner and authored pose");
    expect(!session.land_occupancy()->admits(15, 9, walker), "static building blocks before the first tick");
    auto normalized = take(checkpoint::load_tactical_checkpoint(checkpoint_bytes(session),
        checkpoint::tactical_identities(initial.content_identity), factory(tables), serial));
    expect(normalized.session.canonical_state_bytes() == session.canonical_state_bytes(),
        "unsorted and repeated explicit static cells round trip through normalized occupancy");
    auto static_order = take(TacticalSession::create(initial, tables));
    must(static_order.submit({{0, 1, 50}, {5}, MovePayload{{whole(500), whole(180), {}}}}));
    expect(take(static_order.step(serial)).diagnostics.size() == 1 && static_order.unit_state(5) == structure,
        "owned static roots reject movement and remain fixed during publication");
    // The wider formation at the same building is the #4825 corner case (corner_formation below);
    // a single-member squad proves static occupancy routing on its own.
    auto routed_tables = tables;
    routed_tables.land.companies.front().company.members.resize(1);
    routed_tables.land.companies.front().members.resize(1);
    auto routed = take(TacticalSession::create(initial, routed_tables));
    must(routed.submit({{0, 1, 1}, {1}, MovePayload{{whole(620), whole(180), {}}}}));
    bool detoured = false, crossed = false;
    for (int tick = 0; tick < 360; ++tick) {
        take(routed.step(serial));
        const auto position = routed.land_movement()->squads().front().position;
        detoured = detoured || position.y < whole(140) || position.y > whole(220);
        crossed = crossed || position.x > whole(340);
        const auto column = land_cell_of(position.x, field.terrain->spacing(), field.columns);
        const auto row = land_cell_of(position.y, field.terrain->spacing(), field.rows);
        expect(column && row && std::find(building.primary_cells.begin(), building.primary_cells.end(),
            *row * field.columns + *column) == building.primary_cells.end(),
            "routed squad never enters a static structure's occupied cell");
        expect(routed.unit_state(5) == structure, "static structure remains fixed while squads move");
    }
    expect(detoured && crossed, "squad ordered through blocking structure routes around and passes it");
    expect(!session.submit({{0, 1, 1}, {1}, AttackPayload{3}}), "land command surface excludes combat");
    expect(!session.submit({{0, 1, 1}, {1}, MovePayload{{}, false, true}}), "land waypoint queue is explicitly unavailable");
    auto partial = tables;
    partial.land.companies[0].members[1].type = 0;
    auto survivors = take(TacticalSession::create(initial, partial));
    expect(survivors.land_movement()->members().size() == 8 && survivors.land_movement()->squads().size() == 4,
        "LC-01 failed members leave surviving company members grouped");
    auto space = initial; space.domain = BattleDomain::space;
    auto space_session = take(TacticalSession::create(space));
    const auto space_saved = checkpoint_bytes(space_session);
    const auto space_container = take(checkpoint::parse_checkpoint(space_saved));
    expect(!space_container.find(checkpoint::SectionId::land_state), "space checkpoint contains no land section");
    expect(take(space_session.save_land_checkpoint_state()).empty(), "space raw land checkpoint bytes are absent");
    auto parsed = take(checkpoint::parse_checkpoint(checkpoint_bytes(session)));
    const auto* section = parsed.find(checkpoint::SectionId::land_state);
    expect(section && section->version == 5 && !section->required, "land services use the optional allocated section");
    auto missing = parsed;
    std::erase_if(missing.sections, [](const auto& s) { return s.id == static_cast<std::uint16_t>(checkpoint::SectionId::land_state); });
    expect(!checkpoint::load_tactical_checkpoint(take(checkpoint::write_checkpoint(missing)),
        checkpoint::tactical_identities(initial.content_identity), factory(tables), serial), "land checkpoint refuses a missing state section");
    auto corrupt = parsed;
    corrupt.sections.back().bytes[4] ^= 1;
    expect(!checkpoint::load_tactical_checkpoint(take(checkpoint::write_checkpoint(corrupt)),
        checkpoint::tactical_identities(initial.content_identity), factory(tables), serial), "land checkpoint refuses a different terrain identity");
    auto extra = take(space_session.save_checkpoint_state());
    const auto before = space_session.canonical_state_bytes();
    expect(!space_session.load_land_checkpoint_state(extra) && space_session.canonical_state_bytes() == before,
        "space session refuses a land section without mutation");

    auto accepted = take(TacticalSession::create(initial, tables));
    auto rejected = take(TacticalSession::create(initial, tables));
    const PlayerCommand first_move{{0, 1, 1}, {1}, MovePayload{{whole(620), whole(180), {}}}};
    must(accepted.submit(first_move)); must(rejected.submit(first_move));
    for (std::uint64_t tick : {0U, 1U}) {
        must(rejected.submit({{tick, 1, tick + 2}, {1}, MovePayload{{whole(1000000), whole(1000000), {}}}}));
        take(accepted.step(serial));
        const auto refusal = take(rejected.step(serial));
        expect(refusal.diagnostics.size() == 1 && refusal.diagnostics[0].code == diagnostic_codes::command_rejected,
            "a move without a legal placement reports a command rejection");
        expect(rejected.land_movement()->checkpoint() == accepted.land_movement()->checkpoint()
            && take(rejected.save_land_checkpoint_state()) == take(accepted.save_land_checkpoint_state())
            && rejected.unit_state(1) == accepted.unit_state(1),
            "a refused land move preserves the existing order, pending search and movement");
    }

    const auto baseline = run(initial, tables, serial, false);
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(workers);
        const auto result = run(initial, tables, threads, true);
        expect(result.state == baseline.state && result.checkpoint == baseline.checkpoint,
            "land final state and checkpoints equal on 1/2/4/8 workers with restore boundaries");
        test_support::ReverseOrderExecutor reverse(workers);
        const auto reversed = run(initial, tables, reverse, true);
        expect(reversed.state == baseline.state && reversed.checkpoint == baseline.checkpoint,
            "reverse partition execution preserves full land state and checkpoints");
    }
    test_support::ReverseOrderExecutor observed;
    auto phases = take(TacticalSession::create(initial, tables)); commands(phases);
    std::vector<std::string> names;
    phases.set_commit_observer([&](std::string_view name, bool begin) { if (begin) names.emplace_back(name); });
    take(phases.step(observed));
    const auto path = std::find_if(observed.phases.begin(), observed.phases.end(), [](const auto& p) { return p.name == "land-path-slices"; });
    const auto movement = std::find_if(observed.phases.begin(), observed.phases.end(), [](const auto& p) { return p.name == "land-member-proposals"; });
    expect(path != observed.phases.end() && movement != observed.phases.end() && path < movement,
        "land path slices precede partitioned member proposals");
    expect(names == std::vector<std::string>{"land-occupancy-commit"}, "land has an explicit ordered occupancy commit barrier");

    auto blocked = initial;
    blocked.units[0].position = {whole(100), whole(100), {}};
    blocked.units[1].position = {whole(140), whole(100), {}};
    auto contact_session = take(TacticalSession::create(blocked, tables));
    must(contact_session.submit({{0, 1, 1}, {1}, MovePayload{{whole(300), whole(100), {}}}}));
    bool retained = false;
    for (int i = 0; i < 80; ++i) {
        take(contact_session.step(serial));
        if (contact_session.land_movement()->contacts().empty()) continue;
        retained = true;
        const auto saved = checkpoint_bytes(contact_session);
        auto resumed = take(checkpoint::load_tactical_checkpoint(saved, checkpoint::tactical_identities(blocked.content_identity),
            factory(tables), serial));
        expect(resumed.session.land_movement()->checkpoint() == contact_session.land_movement()->checkpoint(),
            "a nonempty retained contact ledger round trips through the session section");
        for (int j = 0; j < 12; ++j) {
            take(contact_session.step(serial)); take(resumed.session.step(serial));
            expect(contact_session.canonical_state_bytes() == resumed.session.canonical_state_bytes(),
                "restored retained contacts continue identically");
        }
        break;
    }
    expect(retained, "session fixture exercises a moving/stopped retained contact");
}

// #4825: a three-member squad passes a building corner. Blocked members get their own short terrain
// detour (WLPA-69), so the straggler gate (WLPA-30) reopens and the whole squad settles.
struct CornerResult { Bytes state, checkpoint; bool crossed{}, settled{}, detoured{}, squad_route_untouched{true}; };
CornerResult corner_formation(const sim::PartitionExecutor& executor, bool restore, bool reverse) {
    auto field = make_field(std::vector<std::string>(40, std::string(48, '.')));
    auto tables = content(field.terrain);
    auto initial = setup();
    UnitState structure; structure.entity_id = 5; structure.type_id = 13; structure.owner = 1;
    structure.position = {whole(300), whole(180), whole(17)};
    initial.units.push_back(structure);
    if (reverse) initial.units[0].position.x = whole(620);
    tables.land.static_objects.push_back(structure.type_id);
    LandOccupation building; building.object = structure.entity_id; building.primary_passability = wall;
    for (std::uint32_t y = 7; y <= 11; ++y) for (std::uint32_t x = 14; x <= 16; ++x)
        building.primary_cells.push_back(y * field.columns + x);
    tables.land.occupations.push_back(building);
    auto session = take(TacticalSession::create(initial, tables));
    must(session.submit({{0, 1, 1}, {1}, MovePayload{{whole(reverse ? 60 : 620), whole(180), {}}}}));
    const auto identities = checkpoint::tactical_identities(initial.content_identity);
    CornerResult result;
    bool restored = !restore;
    for (int tick = 0; tick < 900; ++tick) {
        take(session.step(executor));
        const auto& squad = session.land_movement()->squads().front();
        result.crossed = result.crossed || (reverse ? squad.position.x < whole(260) : squad.position.x > whole(340));
        result.squad_route_untouched = result.squad_route_untouched && squad.temporary_first == 0;
        bool detouring = false;
        for (const auto& member : session.land_movement()->members())
            detouring = detouring || member.detour_next < member.detour_path.nodes.size();
        result.detoured = result.detoured || detouring;
        if (!restored && detouring) {
            const auto saved = checkpoint_bytes(session);
            auto resumed = take(checkpoint::load_tactical_checkpoint(saved, identities, factory(tables), executor));
            expect(checkpoint_bytes(resumed.session) == saved, "mid-detour land checkpoint bytes round trip");
            session = std::move(resumed.session);
            restored = true;
        }
    }
    const auto& squad = session.land_movement()->squads().front();
    result.settled = squad.order == LandMoveOrder::reached;
    result.state = session.canonical_state_bytes(); result.checkpoint = checkpoint_bytes(session);
    return result;
}

void corner_formation_contracts() {
    const sim::InlineExecutor serial;
    for (const bool reverse : {false, true}) {
        const auto baseline = corner_formation(serial, false, reverse);
        expect(baseline.crossed && baseline.settled, "three-member squad passes the building corner and settles");
        expect(baseline.squad_route_untouched, "corner members never install a squad-wide terrain route");
        std::cout << "land corner formation reverse=" << reverse << " member-detours=" << baseline.detoured << '\n';
        const auto restored = corner_formation(serial, true, reverse);
        expect(restored.state == baseline.state && restored.checkpoint == baseline.checkpoint,
            "save/restore inside a member detour equals uninterrupted motion");
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            platform::ThreadWorkerAdapter threads(workers);
            const auto parallel = corner_formation(threads, false, reverse);
            expect(parallel.state == baseline.state && parallel.checkpoint == baseline.checkpoint,
                "corner formation equals on 1/2/4/8 workers");
            test_support::ReverseOrderExecutor order(workers);
            const auto reversed = corner_formation(order, false, reverse);
            expect(reversed.state == baseline.state && reversed.checkpoint == baseline.checkpoint,
                "corner formation equals with reverse partitions");
        }
    }
}

void stress(bool repulsion = true) {
    auto field = make_field(std::vector<std::string>(128, std::string(128, '.')));
    auto tables = content(field.terrain, 10);
    auto initial = setup(); initial.units.clear();
    for (sim::EntityId id = 1; id <= 80; ++id) {
        UnitState unit; unit.entity_id = id; unit.type_id = 11; unit.owner = id <= 40 ? 1U : 2U;
        unit.position = {whole(60 + static_cast<std::int64_t>((id - 1) % 10) * 180),
            whole(60 + static_cast<std::int64_t>((id - 1) / 10) * 240), {}};
        initial.units.push_back(unit);
    }
    const auto create = [&](bool full_scan) {
        auto session = take(TacticalSession::create(initial, tables));
        // This session owns a mutable LandMovement; the public observer is const.
        // Change only the debug scan policy, outside authoritative checkpoint state.
        const_cast<LandMovement*>(session.land_movement())->set_reference_pair_scans(full_scan);
        const_cast<LandMovement*>(session.land_movement())->set_straggler_repulsion(repulsion);
        for (const auto& unit : initial.units) must(session.submit({{0, unit.owner, unit.entity_id}, {unit.entity_id},
            MovePayload{{whole(2200), unit.position.y, {}}}}));
        return session;
    };
    auto reference = create(true);
    const sim::InlineExecutor serial;
    for (int i = 0; i < 24; ++i) take(reference.step(serial));
    const auto baseline = checkpoint_bytes(reference);
    for (const std::size_t workers : {1U, 2U, 4U, 6U, 8U}) {
        if (!repulsion && workers != 1 && workers != 6) continue;
        auto session = create(false);
        platform::ThreadWorkerAdapter executor(workers, platform::ThreadWorkerAdapter::Dispatch::by_cost);
        core::tick_profile::Recording timing;
        std::string last_hash;
        timing.costs.reserve(24 * 32);
        const auto begin = std::chrono::steady_clock::now();
        {
            core::tick_profile::Capture capture(timing);
            for (int i = 0; i < 24; ++i) last_hash = take(session.step(executor)).state_sha256;
        }
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / 24;
        std::cout << "land session stress objects=880 radius=1 contacts=enabled ticks=24 workers=" << workers << " ms/tick=" << ms;
        std::cout << " mode=" << (repulsion ? "head" : "main-policy");
        double accounted = 0;
        for (const auto name : {"land-path-slices", "land-member-proposals", "land-contacts",
            "land-occupancy-commit", "land-publication"}) {
            double total = 0;
            for (const auto& cost : timing.costs) {
                // Scope rows are exclusive: add the new nested index rows back to
                // their owning wall phase so this table stays comparable to BASE.
                const bool movement_index = std::string_view(name) == "land-member-proposals"
                    && (cost.name == "land-movement-index" || cost.name == "land-movement-index-build");
                const bool contact_index = std::string_view(name) == "land-contacts"
                    && (cost.name == "land-contact-index" || cost.name == "land-contact-index-build");
                if (cost.name == name || movement_index || contact_index) total += cost.ms;
            }
            const auto per_tick = total / 24;
            accounted += per_tick;
            std::cout << ' ' << name << '=' << per_tick;
        }
        std::cout << " serial-remainder=" << std::max(0.0, ms - accounted);
        for (const auto name : {"land-serial-prepare", "land-canonical-state", "land-state-hash",
            "land-movement-index-build", "land-contact-index-build",
            "serial.tactical.finish.records", "serial.tactical.finish.encoding"}) {
            double total = 0;
            for (const auto& cost : timing.costs) if (cost.name == name) total += cost.ms;
            std::cout << ' ' << name << '=' << total / 24;
        }
        std::cout << '\n';
        expect(session.snapshot()->instances().size() == 880, "stress exercises 880 land objects");
        expect(last_hash == session.state_sha256(),
            "partitioned land canonical encoding hashes like the serial encoding");
        expect(checkpoint_bytes(session) == baseline, "indexed stress final checkpoint equals old full scans on all worker counts");
        test_support::ReverseOrderExecutor reverse(workers);
        auto reversed = create(false);
        for (int i = 0; i < 24; ++i) take(reversed.step(reverse));
        expect(checkpoint_bytes(reversed) == baseline, "indexed stress checkpoint equals old full scans with reverse partitions");
    }
}

int installed() {
    const auto root = testing::environment("EAWR_EAW_GAME_ROOT");
    if (!root || root->empty()) { std::cout << "SKIPPED: no game data\n"; return 77; }
    std::vector<vfs::MountSpec> mounts;
    for (const auto* folder : {"corruption", "GameData"})
        mounts.push_back(take(vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data")).mount);
    auto filesystem = take(vfs::Vfs::mount(mounts));
    const auto map = take(assets::load_map(filesystem, "data/art/maps/_mp_land_naboo.ted"));
    auto terrain = std::make_shared<const LandTerrain>(take(skirmish::load_land_terrain(map, {{"walker", 0xffffffffU}})));
    auto tables = content(terrain);
    auto initial = setup();
    // Use authored cell centres safely inside the actual decoded grid; no local retail data is read.
    for (std::size_t i = 0; i < initial.units.size(); ++i) {
        initial.units[i].position.x = land_cell_centre(terrain->width() / 3, terrain->spacing());
        initial.units[i].position.y = land_cell_centre(terrain->height() / 3 + static_cast<std::uint32_t>(i) * 3, terrain->spacing());
    }
    const sim::InlineExecutor serial;
    auto reference = take(TacticalSession::create(initial, tables));
    // The fixture's commands remain synthetic; the field is the stock map's verified decoded terrain.
    const auto queue = [&](TacticalSession& session) {
        for (const auto& unit : initial.units) must(session.submit({{0, unit.owner, unit.entity_id}, {unit.entity_id},
            MovePayload{{land_cell_centre(terrain->width() * 2 / 3, terrain->spacing()), unit.position.y, {}}}}));
    };
    queue(reference);
    for (int i = 0; i < 64; ++i) take(reference.step(serial));
    const auto baseline = reference.canonical_state_bytes();
    const auto saved_baseline = checkpoint_bytes(reference);
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        test_support::ReverseOrderExecutor reverse(workers);
        for (const auto* runner : std::array<const sim::PartitionExecutor*, 2>{&executor, &reverse}) {
            auto session = take(TacticalSession::create(initial, tables)); queue(session);
            for (int i = 0; i < 64; ++i) {
                take(session.step(*runner));
                if (i == 0 || i == 7 || i == 31) {
                    auto resumed = take(checkpoint::load_tactical_checkpoint(checkpoint_bytes(session),
                        checkpoint::tactical_identities(initial.content_identity), factory(tables), *runner));
                    session = std::move(resumed.session);
                }
            }
            expect(session.canonical_state_bytes() == baseline && checkpoint_bytes(session) == saved_baseline,
                "stock land terrain session is equal across workers, reverse partitions and direct restores");
        }
    }
    auto playback = take(TacticalSession::from_replay(take(parse_replay(take(write_replay(reference.record())))), tables));
    for (int i = 0; i < 64; ++i) take(playback.step(serial));
    expect(playback.canonical_state_bytes() == baseline, "stock land terrain session replay round trips");
    std::cout << "stock land session map=data/art/maps/_mp_land_naboo.ted width=" << terrain->width() << " height=" << terrain->height() << '\n';
    return checks.finish();
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") return installed();
        contracts(); live_contracts(); corner_formation_contracts(); stress(false); stress(); return checks.finish();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
