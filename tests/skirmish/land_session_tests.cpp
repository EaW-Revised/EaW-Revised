#include "eawr/checkpoint/tactical.hpp"
#include "eawr/core/tick_profile.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/presentation/live/land_ground.hpp"
#include "eawr/skirmish/land_fog.hpp"
#include "eawr/skirmish/land_terrain.hpp"
#include "eawr/vfs/vfs.hpp"
#include "land_test_support.hpp"
#include "../support/env.hpp"
#include "../support/reverse_partition_executor.hpp"
#include "../../src/sim/tactical/checkpoint_fields_internal.hpp"
#include "../../src/sim/tactical/land_checkpoint_fields_internal.hpp"

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

TacticalContent content(std::shared_ptr<const LandTerrain> terrain, std::size_t member_count = 3, Fixed turn_rate = whole(2)) {
    TacticalContent result;
    result.domain = BattleDomain::land; result.land_terrain = std::move(terrain);
    result.land.clear_bit = clear;
    result.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), whole(1)};
    LandCompanyBinding binding;
    binding.type = 11; binding.company.id = "synthetic-company";
    binding.company.team_max_speed = whole(60); binding.company.max_squad_size = static_cast<std::uint32_t>(member_count);
    binding.company.team_collision_class = units::LandCollisionGroup::infantry; // WLPA-82: the container's class makes the squad a body
    binding.effective_team_max_speed = whole(60);
    binding.formation_spacing = Fixed::from_ratio(1, 5).value();
    for (std::size_t i = 0; i < member_count; ++i) {
        units::LandCompanyMember member;
        member.type = "synthetic-member"; member.kind = units::LandObjectKind::infantry;
        member.create_team = true; member.movement_class = "walker"; member.max_speed = whole(80);
        member.locomotor = "synthetic-walking";
        binding.company.members.push_back(member);
        binding.members.push_back({12, {whole(80), whole(120), whole(150), turn_rate, whole(1), false, eawr::units::LandCollisionGroup::infantry}, 0, {}, {}, {}});
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

Bytes checkpoint_bytes(const TacticalSession& session);
void victory_contracts() {
    auto input = setup();
    input.units.resize(2);
    input.units[0].position = {whole(100), whole(100), {}};
    input.units[1].owner = 2;
    input.units[1].position = {whole(190), whole(100), {}};
    auto field = make_field(std::vector<std::string>(40, std::string(64, '.')));
    auto rules = content(field.terrain, 1);
    rules.victory.condition = VictoryCondition::all_enemy_units_destroyed;
    rules.victory.relevant_types = {11};
    rules.victory.contenders = {1, 2};
    rules.victory.controlled_players = {1, 2};
    rules.victory.installed_players = {1, 2};
    DamageRules damage;
    damage.shield_recharge_frames = 30; damage.damage_types = 1; damage.armor_types = 1;
    damage.armor_mods = {whole(1)}; rules.durability.damage = damage;
    DurabilityProfile health;
    health.type_id = 12; health.max_hull = whole(4); health.allow_diminishing_firepower = false;
    rules.durability.profiles.push_back(health);
    CombatProfile gun;
    gun.type_id = 12; gun.max_attack_distance = whole(150); gun.living_projectile_collision = true;
    gun.collision = CollisionBox{{whole(-2), whole(-2), whole(-2)}, {whole(2), whole(2), whole(4)}};
    WeaponProfile weapon;
    weapon.range = whole(150); weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 100;
    weapon.pulse_count = 1;
    ShotProfile shot; shot.damage = whole(4); shot.speed = whole(4); shot.max_travel = whole(500);
    weapon.shot = shot; gun.weapons.push_back(weapon); rules.combat.profiles.push_back(gun);
    rules.combat.footprints.push_back({12, whole(2), whole(2), whole(2)});
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        auto session = take(TacticalSession::create(input, rules));
        must(session.submit({{0, 1, 1}, {1}, AttackPayload{2}}));
        must(session.submit({{0, 2, 1}, {2}, AttackPayload{1}}));
        std::vector<std::string> hashes;
        bool restored = false;
        for (int frame = 0; frame < 300; ++frame) {
            const auto tick = take(session.step(executor));
            hashes.push_back(tick.state_sha256);
            if (session.outcome() && !restored) {
                const auto bytes = checkpoint_bytes(session);
                auto loaded = take(checkpoint::load_tactical_checkpoint(bytes,
                    checkpoint::tactical_identities(input.content_identity),
                    [&rules](const TacticalReplay& replay) { return TacticalSession::from_replay(replay, rules); }, executor));
                session = std::move(loaded.session);
                expect(checkpoint_bytes(session) == bytes, "pending land victory restores exactly");
                restored = true;
            }
        }
        expect(restored && session.outcome() && session.outcome()->winner >= 1 && session.outcome()->winner <= 2
            && session.outcome()->end_tick <= session.completed_tick(), "mutual fire retains the first winner through battle end");
        if (reference.empty()) reference = std::move(hashes);
        else expect(hashes == reference, "land victory digests agree on 1/2/4/8 workers");
    }
    // One unarmed enemy and a distant allied component exercise the LC-70 relationship filter.
    input.players.push_back({3, 1, 3, player_flag_commandable});
    input.units[1].type_id = 22;
    UnitState ally = input.units[0]; ally.entity_id = 3; ally.owner = 3;
    ally.position = {whole(500), whole(500), {}}; input.units.push_back(ally);
    rules.land.static_objects.push_back(22);
    LandOccupation target_occupation; target_occupation.object = 2;
    rules.land.occupations.push_back(target_occupation);
    auto target_health = health; target_health.type_id = 22; rules.durability.profiles.push_back(target_health);
    auto target_body = gun; target_body.type_id = 22; target_body.weapons.clear();
    rules.combat.profiles.push_back(target_body);
    rules.combat.footprints.push_back({22, whole(2), whole(2), whole(2)});
    rules.victory.condition = VictoryCondition::enemy_base_components_destroyed;
    rules.victory.starbase_types = {11, 22};
    rules.victory.contenders = {1, 2, 3}; rules.victory.controlled_players = {1, 2, 3};
    rules.victory.installed_players = {1, 2, 3};
    sim::InlineExecutor executor;
    auto base = take(TacticalSession::create(input, rules));
    expect(players_allied(base.snapshot()->players(), 1, 3)
        && players_hostile(base.snapshot()->players(), 1, 2), "LC-70 fixture has the intended player relationships");
    expect(!base.outcome(), "LC-70 is event driven at battle start");
    must(base.submit({{0, 1, 1}, {1}, AttackPayload{2}}));
    bool target_destroyed = false;
    for (int frame = 0; frame < 180 && !base.outcome(); ++frame) {
        const auto step = take(base.step(executor));
        for (const auto& event : step.snapshot->events())
            target_destroyed |= event.kind == EventKind::unit_destroyed && event.unit == 2;
    }
    expect(target_destroyed && base.outcome() && base.outcome()->winner == 1 && base.unit_state(3),
        "last enemy component ends battle while an allied component remains");
    rules.victory.starbase_types.clear();
    auto no_components = take(TacticalSession::create(input, rules));
    must(no_components.submit({{0, 1, 1}, {1}, AttackPayload{2}}));
    for (int frame = 0; frame < 180; ++frame) take(no_components.step(executor));
    expect(!no_components.outcome(), "LC-70 never ends a component-free map at start or on unrelated deaths");
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
    const auto motion = live->land_motion();
    expect(motion && motion->tick == 90 && motion->members.size() == live->land_roster().size(),
        "land motion view copies the completed member state");
    if (motion) for (const auto& member : motion->members) {
        expect(member.entity != 0 && !member.spread_out && !member.stop_point,
            "land motion view retains identity and the unpublished inputs default false");
    }
    const auto motion_history = live->land_motion_after(0);
    expect(motion_history.size() == 90 && motion_history.front().tick == 1 && motion_history.back().tick == 90,
        "land motion history preserves each completed tick in order");
    expect(std::any_of(motion_history.begin(), motion_history.end(), [](const auto& frame) {
        return std::any_of(frame.members.begin(), frame.members.end(), [](const auto& member) {
            return member.has_path_target && member.turn_rate_limit.raw() > 0;
        });
    }), "land motion history carries the current path target and maximum turn rate");
    std::vector<platform::LiveLandMotion> reused_motion;
    const auto partial_count = live->land_motion_through(0, 2, reused_motion);
    expect(partial_count == 2 && reused_motion[0].tick == 1 && reused_motion[1].tick == 2,
        "bounded motion sampling stops at the presented tick");
    const auto* retained_members = reused_motion[0].members.data();
    const auto repeat_count = live->land_motion_through(0, 1, reused_motion);
    expect(repeat_count == 1 && reused_motion[0].members.data() == retained_members,
        "bounded motion sampling reuses roster storage");
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
    expect(static_cast<bool>(session.submit({{0, 1, 1}, {1}, AttackPayload{3}})), "land command surface accepts attack");
    expect(!session.submit({{0, 1, 2}, {1}, AttackMovePayload{}}), "land attack-move remains explicitly unavailable");
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
    expect(section && section->version == checkpoint::land_state_section_version && !section->required,
        "land services use the optional allocated section");

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
        auto counted = contact_session.land_movement()->checkpoint();
        const auto before_counts = contact_session.state_sha256();
        counted.contacts.front().retry_remaining = counted.contacts.front().retry_remaining == 15 ? 14 : 15;
        *const_cast<LandMovement*>(contact_session.land_movement()) = LandMovement::from_checkpoint(counted).value();
        expect(contact_session.state_sha256() != before_counts, "contact retry contributes to the canonical hash");
        auto hash = contact_session.state_sha256();
        counted.squads.front().straggler_wait_remaining = counted.squads.front().straggler_wait_remaining == 14 ? 13 : 14;
        *const_cast<LandMovement*>(contact_session.land_movement()) = LandMovement::from_checkpoint(counted).value();
        expect(contact_session.state_sha256() != hash, "straggler count contributes to the canonical hash");
        hash = contact_session.state_sha256();
        counted.members.front().straggler_best_lag = whole(19);
        *const_cast<LandMovement*>(contact_session.land_movement()) = LandMovement::from_checkpoint(counted).value();
        expect(contact_session.state_sha256() != hash, "straggler best lag contributes to the canonical hash");
        const auto saved = checkpoint_bytes(contact_session);
        auto resumed = take(checkpoint::load_tactical_checkpoint(saved, checkpoint::tactical_identities(blocked.content_identity),
            factory(tables), serial));
        expect(resumed.session.land_movement()->checkpoint() == contact_session.land_movement()->checkpoint(),
            "a nonempty retained contact ledger round trips through the session section");
        expect(resumed.session.land_movement()->contacts().front().retry_remaining == counted.contacts.front().retry_remaining
            && resumed.session.land_movement()->squads().front().straggler_wait_remaining == counted.squads.front().straggler_wait_remaining
            && resumed.session.land_movement()->members().front().straggler_best_lag == whole(19),
            "checkpoint bytes restore both guards and best lag mid-count");
        for (int j = 0; j < 12; ++j) {
            take(contact_session.step(serial)); take(resumed.session.step(serial));
            expect(contact_session.canonical_state_bytes() == resumed.session.canonical_state_bytes(),
                "restored retained contacts continue identically");
        }
        break;
    }
    expect(retained, "session fixture exercises a moving/stopped retained contact");
}

// #4825 / WLPA-36/69: a three-member squad passes a building corner. Only the container tests its centre point; a soldier
// whose slot falls into the blocked belt walks straight through it (retail clips corners) and holds nothing.
struct CornerResult { Bytes state, checkpoint; bool crossed{}, settled{}, clipped{}, squad_route_untouched{true}; };
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
        for (const auto& member : session.land_movement()->members()) {
            const auto column = land_cell_of(member.position.x, field.terrain->spacing(), field.columns);
            const auto row = land_cell_of(member.position.y, field.terrain->spacing(), field.rows);
            result.clipped = result.clipped || !column || !row || std::find(building.primary_cells.begin(),
                building.primary_cells.end(), *row * field.columns + *column) != building.primary_cells.end();
        }
        if (!restored && tick == 150) {
            const auto saved = checkpoint_bytes(session);
            auto resumed = take(checkpoint::load_tactical_checkpoint(saved, identities, factory(tables), executor));
            expect(checkpoint_bytes(resumed.session) == saved, "mid-corner land checkpoint bytes round trip");
            session = std::move(resumed.session);
            restored = true;
        }
    }
    const auto& squad = session.land_movement()->squads().front();
    result.settled = squad.order == LandMoveOrder::reached;
    result.state = session.canonical_state_bytes(); result.checkpoint = checkpoint_bytes(session);
    return result;
}

// WLPA-88/89: a move order given to a squad that is walking does not stop it. The result of the search from the end of
// its bounded segment is spliced in; the corner keeps the walk at its turn speed (an authored minimum fraction would stop it).
struct ReorderResult { Bytes state, checkpoint; bool never_stopped{true}, turned{}, reached{}, old_end{}; };
ReorderResult reorder_while_walking(const sim::PartitionExecutor& executor, bool restore, Fixed turn_rate, bool backward = false) {
    auto field = make_field(std::vector<std::string>(40, std::string(48, '.')));
    const auto tables = content(field.terrain, 3, turn_rate);
    const auto initial = setup();
    auto session = take(TacticalSession::create(initial, tables));
    must(session.submit({{0, 1, 1}, {1}, MovePayload{{whole(620), whole(180), {}}}}));
    must(session.submit({{60, 1, 20}, {1}, MovePayload{{whole(backward ? 60 : 400), whole(backward ? 180 : 500), {}}}}));
    const auto identities = checkpoint::tactical_identities(initial.content_identity);
    ReorderResult result;
    for (int tick = 0; tick < 1100; ++tick) {
        const auto before = session.land_movement()->squads().front().position;
        take(session.step(executor));
        const auto& squad = session.land_movement()->squads().front();
        if (tick >= 60 && tick < 90 && turn_rate >= whole(1))
            result.never_stopped = result.never_stopped && squad.position != before && squad.order == LandMoveOrder::following;
        if (tick >= 60) result.turned = result.turned || squad.turning;
        if (tick >= 60) result.old_end = result.old_end || squad.position.x >= whole(610);
        if (restore && tick % 37 == 0) {
            const auto saved = checkpoint_bytes(session);
            auto resumed = take(checkpoint::load_tactical_checkpoint(saved, identities, factory(tables), executor));
            expect(checkpoint_bytes(resumed.session) == saved, "land checkpoint bytes round trip across the re-order");
            session = std::move(resumed.session);
        }
    }
    const auto& squad = session.land_movement()->squads().front();
    result.reached = squad.order == LandMoveOrder::reached && (backward ? squad.position.x < whole(100) : squad.position.y > whole(400));
    result.state = session.canonical_state_bytes(); result.checkpoint = checkpoint_bytes(session);
    return result;
}

void reorder_contracts() {
    const sim::InlineExecutor serial;
    for (const auto turn_rate : {whole(2), Fixed::from_ratio(1, 10).value()}) {
        const bool smooth = turn_rate >= whole(1);
        const auto baseline = reorder_while_walking(serial, false, turn_rate);
        expect(baseline.reached, "the re-ordered squad reaches the new destination");
        expect(!baseline.old_end, "a perpendicular re-order does not reach the old endpoint");
        if (smooth) expect(baseline.never_stopped && !baseline.turned,
            "WLPA-88 a smooth re-order keeps the squad walking: it never stops or turns in place");
        else expect(baseline.reached && !baseline.turned,
            "WLPA-89 a slow turn rate slows the corner (WLPA-90) but never falls back to a stop without an authored minimum");
        const auto restored = reorder_while_walking(serial, true, turn_rate);
        expect(restored.state == baseline.state && restored.checkpoint == baseline.checkpoint,
            "save/restore across the re-order equals uninterrupted motion");
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            platform::ThreadWorkerAdapter threads(workers);
            const auto parallel = reorder_while_walking(threads, false, turn_rate);
            expect(parallel.state == baseline.state && parallel.checkpoint == baseline.checkpoint, "re-order equals on 1/2/4/8 workers");
            test_support::ReverseOrderExecutor order(workers);
            const auto reversed = reorder_while_walking(order, false, turn_rate);
            expect(reversed.state == baseline.state && reversed.checkpoint == baseline.checkpoint, "re-order equals with reverse partitions");
        }
    }
    const auto backward = reorder_while_walking(serial, false, whole(2), true);
    expect(backward.reached && !backward.old_end, "a backward re-order does not walk to the old endpoint");
}

// Inspect the archived searches so this fixture proves the save is inside the search, rather than just near the order.
std::map<sim::EntityId, LandSearch::Checkpoint> archived_land_searches(const TacticalSession& session) {
    const auto bytes = take(session.save_land_checkpoint_state());
    checkpoint_detail::Reader in(bytes);
    in.fire_range_bonuses = true; in.script_invulnerable = true; // hidden garrison occupants carry whole live units (version 16)
    std::uint16_t id{}, version{};
    std::array<std::uint8_t, 32> terrain{};
    std::uint32_t clear_bit{};
    LandOccupancy::Checkpoint occupancy;
    LandMovement::Checkpoint movement;
    std::map<sim::EntityId, LandPathRequest> requests;
    session_detail::LandCombatState combat;
    std::vector<sim::EntityId> dynamic_children; // the economy's created objects precede the searches (version 11)
    std::map<sim::EntityId, LandFormationOrder> formations; // the pending formation orders follow them (version 12)
    std::vector<session_detail::LandLanding> landings; // version 13: companies on their way down, the landed ids and the counter
    std::vector<sim::EntityId> landed;
    std::uint64_t next_landing{};
    std::map<sim::EntityId, session_detail::LandWalkingFormation> walking_formations; // version 15
    std::map<sim::EntityId, session_detail::LandGarrisonEntry> garrison_entries; // version 16: the garrison records follow the walking formations
    in(id, version, terrain, clear_bit, occupancy, movement, requests, combat, dynamic_children, formations, landings, landed,
        next_landing, walking_formations, garrison_entries);
    std::map<sim::EntityId, LandSearch::Checkpoint> searches;
    const auto count = in.count();
    for (std::uint64_t i = 0; i < count; ++i) {
        sim::EntityId object{};
        LandSearch::Checkpoint search;
        in(object, search);
        searches.emplace(object, std::move(search));
    }
    return searches;
}

struct MazeReorderResult { Bytes state, checkpoint; bool saved_in_flight{}, restarted{}, old_end{}, reached{}, cut{true}; };
MazeReorderResult maze_reorder(const sim::PartitionExecutor& executor, bool restore, bool repeated = false) {
    std::vector<std::string> picture(128, std::string(128, '.'));
    for (int row = 20, wall_index = 0; row <= 110; row += 15, ++wall_index) {
        for (int col = 0; col < 128; ++col) picture[row][col] = '#';
        const int gap = wall_index % 2 == 0 ? 4 : 118;
        for (int col = gap; col < gap + 6; ++col) picture[row][col] = '.';
    }
    const auto field = make_field(picture);
    const auto tables = content(field.terrain, 1);
    auto initial = setup();
    initial.units.resize(1);
    auto session = take(TacticalSession::create(initial, tables));
    must(session.submit({{0, 1, 1}, {1}, MovePayload{{whole(2300), whole(180), {}}}}));
    must(session.submit({{60, 1, 2}, {1}, MovePayload{{whole(2200), whole(2300), {}}}}));
    if (repeated) must(session.submit({{70, 1, 3}, {1}, MovePayload{{whole(2100), whole(2300), {}}}}));
    const auto identities = checkpoint::tactical_identities(initial.content_identity);
    MazeReorderResult result;
    std::optional<sim::math::Vec2> first_anchor;
    std::uint64_t previous_work = 0;
    for (int tick = 0; tick < 15000; ++tick) {
        take(session.step(executor));
        if (tick < 60) continue;
        const auto& squad = session.land_movement()->squads().front();
        result.old_end = result.old_end || (squad.position.x >= whole(2290) && squad.position.y < whole(200));
        if (tick == 60 || (repeated && tick == 70))
            result.cut = result.cut && squad.path.nodes.size() == squad.next_node + 1;
        if (tick % 5 != 0) continue;
        const auto searches = archived_land_searches(session);
        const auto search = searches.find(1);
        if (search != searches.end()) {
            if (!first_anchor) first_anchor = search->second.request.start;
            else if (search->second.request.start != *first_anchor && (!repeated || tick > 70)) result.restarted = true;
            if ((!repeated || tick != 70) && search->second.stats.work < previous_work) result.restarted = true;
            previous_work = search->second.stats.work;
            if (tick == 65 && search->second.stats.work > 256) {
                result.saved_in_flight = true;
                if (restore) {
                    const auto saved = checkpoint_bytes(session);
                    auto resumed = take(checkpoint::load_tactical_checkpoint(saved, identities, factory(tables), executor));
                    expect(checkpoint_bytes(resumed.session) == saved, "in-flight re-order checkpoint round trips");
                    session = std::move(resumed.session);
                }
            }
        }
        const auto& current = session.land_movement()->squads().front();
        if (current.order == LandMoveOrder::reached && current.position.y > whole(2200)) {
            result.reached = current.position.x == whole(repeated ? 2100 : 2200);
            break;
        }
    }
    result.state = session.canonical_state_bytes(); result.checkpoint = checkpoint_bytes(session);
    return result;
}

void maze_reorder_contracts() {
    const sim::InlineExecutor serial;
    const auto baseline = maze_reorder(serial, false);
    const auto restored = maze_reorder(serial, true);
    expect(baseline.saved_in_flight && restored.saved_in_flight, "maze re-order search spans several ticks at save");
    expect(!baseline.restarted && !baseline.old_end && baseline.reached && baseline.cut,
        "long search does not restart or reach the old end, and reaches the new destination");
    expect(restored.state == baseline.state && restored.checkpoint == baseline.checkpoint,
        "in-flight re-order save and restore matches uninterrupted session");
    const auto repeated = maze_reorder(serial, false, true);
    expect(repeated.saved_in_flight && !repeated.restarted && !repeated.old_end && repeated.reached && repeated.cut,
        "repeated long-search re-order cuts old nodes and the last order wins without a restart");
}

void corner_formation_contracts() {
    const sim::InlineExecutor serial;
    for (const bool reverse : {false, true}) {
        const auto baseline = corner_formation(serial, false, reverse);
        expect(baseline.crossed && baseline.settled, "three-member squad passes the building corner and settles");
        expect(baseline.squad_route_untouched, "corner soldiers never install a squad-wide terrain route");
        std::cout << "land corner formation reverse=" << reverse << " soldiers-clipped=" << baseline.clipped << '\n';
        const auto restored = corner_formation(serial, true, reverse);
        expect(restored.state == baseline.state && restored.checkpoint == baseline.checkpoint,
            "save/restore at the corner equals uninterrupted motion");
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


// LC-54..57 (#5893): the land step runs the shared tactical economy and pad services on authored profiles.
constexpr sim::tactical::TypeId pad_type = 20, station_type = 21, site_type = 30, turret_type = 40, upgrade_type = 50;
struct EconomyCase { TacticalSetup setup; TacticalContent content; };

// A pad (id 5) and an income station (id 6), both static roots of player 1 with their own occupation, on any terrain.
EconomyCase economy_case(const std::shared_ptr<const LandTerrain>& terrain, const std::uint32_t pad_column,
    const std::uint32_t pad_row, const std::uint32_t station_row) {
    EconomyCase result;
    result.content = content(terrain, 1);
    result.setup = setup();
    result.setup.players.push_back({3, 3, 3, 0}); // the declared neutral player pad capture requires
    const auto place = [&](sim::EntityId id, TypeId type, std::uint32_t row) {
        UnitState unit; unit.entity_id = id; unit.type_id = type; unit.owner = 1;
        unit.position = {land_cell_centre(pad_column, terrain->spacing()), land_cell_centre(row, terrain->spacing()), whole(17)};
        result.setup.units.push_back(unit);
        result.content.land.static_objects.push_back(type);
        LandOccupation occupation; occupation.object = id; occupation.primary_passability = wall;
        for (std::uint32_t y = row - 1; y <= row + 1; ++y) for (std::uint32_t x = pad_column - 1; x <= pad_column + 1; ++x)
            occupation.primary_cells.push_back(y * terrain->width() + x);
        result.content.land.occupations.push_back(std::move(occupation));
    };
    place(5, pad_type, pad_row);
    place(6, station_type, station_row);
    for (const TypeId type : {pad_type, station_type, site_type, turret_type}) {
        DurabilityProfile health; health.type_id = type; health.max_hull = whole(type == turret_type ? 600 : 300);
        result.content.durability.profiles.push_back(health);
    }
    auto& economy = result.content.economy;
    economy.players = {{1, whole(1000), 0}, {2, whole(1000), 0}};
    StationMenu pad_menu; pad_menu.station = pad_type; pad_menu.faction = 1; pad_menu.station_producer = false;
    pad_menu.options = {{site_type, BuildKind::structure, BuildQueue::units, whole(100), 30, 30, 0, true}};
    StationMenu station_menu; station_menu.station = station_type; station_menu.faction = 1;
    BuildOption upgrade; upgrade.type = upgrade_type; upgrade.kind = BuildKind::upgrade; upgrade.queue = BuildQueue::upgrades;
    upgrade.price = whole(200); upgrade.build_frames = upgrade.ai_build_frames = 60; upgrade.available = true;
    station_menu.options = {upgrade};
    economy.menus = {pad_menu, station_menu};
    UpgradeProfile profile; profile.type = upgrade_type; economy.upgrades = {profile};
    IncomeProfile stream; stream.source = station_type; stream.per_frame = Fixed::from_raw(Fixed::scale); economy.income = {stream};
    economy.pads.neutral = 3;
    economy.pads.capture = {{pad_type, whole(100), whole(5), {1, 2}, false, true, true}};
    economy.pads.construction = {{site_type, turret_type, whole(100), 1, 1}};
    economy.pads.influence = {{pad_type, false}, {site_type, false}, {turret_type, false}};
    return result;
}

std::size_t count_events(const TacticalTick& tick, EventKind kind) {
    return static_cast<std::size_t>(std::count_if(tick.snapshot->events().begin(), tick.snapshot->events().end(),
        [kind](const auto& event) { return event.kind == kind; }));
}

struct EconomyOutcome { Bytes state, checkpoint; };
EconomyOutcome economy_run(const EconomyCase& input, const sim::PartitionExecutor& executor, const bool restore) {
    auto session = take(TacticalSession::create(input.setup, input.content));
    expect(session.ledgers().size() == 2 && session.ledgers()[0].credits == whole(1000) && session.pads().size() == 1
        && session.pads().begin()->first == 5, "LC-54: the land session opens its ledgers and registers the pad from the authored rows");
    // Player 1 builds on its pad; player 2 cannot (no menu of its faction on that pad); the upgrade is bought, cancelled
    // with a full refund, and bought again.
    must(session.submit({{0, 1, 1}, {5}, PadBuildPayload{site_type}}));
    must(session.submit({{0, 2, 1}, {5}, PadBuildPayload{site_type}}));
    must(session.submit({{3, 1, 2}, {6}, BuyPayload{upgrade_type}}));
    must(session.submit({{6, 1, 3}, {}, CancelPayload{1, 0}}));
    must(session.submit({{8, 1, 4}, {6}, BuyPayload{upgrade_type}}));
    const auto identities = checkpoint::tactical_identities(input.setup.content_identity);
    sim::EntityId site = 0;
    for (std::uint64_t tick = 0; tick < 120; ++tick) {
        const auto stepped = take(session.step(executor));
        const auto completed = tick + 1;
        const auto& player = session.ledgers()[0];
        if (tick == 0) {
            expect(count_events(stepped, EventKind::pad_construction_started) == 1 && stepped.diagnostics.size() == 1
                && stepped.diagnostics[0].code == diagnostic_codes::command_rejected,
                "LC-55: the pad build starts and the other faction's request is rejected with a diagnostic");
            site = session.pads().at(5).under_construction;
            expect(site != 0 && session.construction().size() == 1 && session.unit_state(site)
                && session.unit_state(site)->type_id == site_type && session.unit_state(site)->owner == 1,
                "LC-55: the construction site exists as a unit of the builder");
        }
        if (completed == 1) expect(player.credits == whole(901), "the price is debited once and one frame of income is paid");
        if (completed == 5) expect(player.queues[1].size() == 1 && player.credits == whole(1000 - 100 - 200 + 5),
            "the upgrade purchase debits its price into the upgrade queue");
        if (completed == 8) expect(player.queues[1].empty() && player.credits == whole(1000 - 100 + 8),
            "cancelling the queued upgrade refunds its full price");
        if (completed == 30) expect(session.pads().at(5).constructed == 0 && session.pads().at(5).under_construction == site,
            "the site stays under construction until its deadline");
        if (completed == 31) {
            const auto built = session.pads().at(5).constructed;
            expect(count_events(stepped, EventKind::pad_construction_completed) == 1 && built != 0 && built != site
                && session.pads().at(5).under_construction == 0 && session.construction().empty()
                && !session.unit_state(site) && session.unit_state(built) && session.unit_state(built)->type_id == turret_type
                && session.unit_state(built)->owner == 1 && session.durability_state(built)->hull == whole(600),
                "LC-55: completion replaces the site by the constructed turret with the hull the site grew");
        }
        if (restore && (tick == 0 || tick == 10 || tick == 29 || tick == 30 || tick == 70)) {
            const auto saved = checkpoint_bytes(session);
            auto resumed = take(checkpoint::load_tactical_checkpoint(saved, identities, factory(input.content), executor));
            expect(resumed.work.resimulated_ticks == 0 && checkpoint_bytes(resumed.session) == saved,
                "a land checkpoint inside a build, a queue or after completion restores its created objects exactly");
            session = std::move(resumed.session);
        }
    }
    const auto& owner = session.ledgers()[0];
    expect(owner.credits == whole(1000 + 120 - 100 - 200) && owner.queues[1].empty() && owner.completed.size() == 1
        && owner.completed[0].type == upgrade_type && session.ledgers()[1].credits == whole(1000),
        "income streams pay the owner every frame and the second upgrade completes");
    auto replay = take(parse_replay(take(write_replay(session.record()))));
    auto playback = take(TacticalSession::from_replay(replay, input.content));
    for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) take(playback.step(executor));
    expect(playback.canonical_state_bytes() == session.canonical_state_bytes(), "the economy routes replay to the same land state");
    return {session.canonical_state_bytes(), checkpoint_bytes(session)};
}

// The enemy squad (ids 3 and 4) stands 130 units north of the pad, inside gun range, and kills what is built on it.
// The player's own squads (ids 1 and 2) are far away. A shot does 6 and a built object has 8 hull, so it takes two hits.
EconomyCase battle_case(const std::shared_ptr<const LandTerrain>& terrain, const std::uint32_t seconds, const bool pad_targetable) {
    auto result = economy_case(terrain, 15, 9, 15);
    auto& economy = result.content.economy;
    economy.pads.construction.front().seconds = seconds;
    economy.pads.capture.front().rebuild_frames = 15; // LC-56: the rebuild cooldown, 15 frames in this fixture
    for (auto& unit : result.setup.units) if (unit.entity_id == 5 || unit.entity_id == 6) unit.position.z = {};
    result.setup.units[0].position = {whole(60), whole(700), {}};
    result.setup.units[1].position = {whole(60), whole(740), {}};
    result.setup.units[2].position = {whole(310), whole(60), {}};
    result.setup.units[3].position = {whole(350), whole(60), {}};
    auto& rules = result.content;
    DamageRules damage;
    damage.shield_recharge_frames = 30; damage.damage_types = 1; damage.armor_types = 1; damage.armor_mods = {whole(1)};
    rules.durability.damage = damage;
    rules.durability.profiles.clear();
    for (const TypeId type : std::array<TypeId, 5>{12, pad_type, station_type, site_type, turret_type}) {
        DurabilityProfile health; health.type_id = type; health.allow_diminishing_firepower = false;
        health.max_hull = whole(type == 12 ? 4 : type == station_type || (type == pad_type && !pad_targetable) ? 300 : 8);
        rules.durability.profiles.push_back(health);
    }
    CombatProfile gun;
    gun.type_id = 12; gun.max_attack_distance = whole(150); gun.living_projectile_collision = true;
    gun.collision = CollisionBox{{whole(-2), whole(-2), whole(-2)}, {whole(2), whole(2), whole(4)}};
    WeaponProfile weapon;
    weapon.range = whole(150); weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 100;
    weapon.pulse_count = 1;
    ShotProfile shot; shot.damage = whole(6); shot.speed = whole(4); shot.max_travel = whole(500);
    weapon.shot = shot; gun.weapons.push_back(weapon);
    rules.combat.profiles.push_back(gun);
    rules.combat.footprints.push_back({12, whole(2), whole(2), whole(2)});
    for (const TypeId type : std::array<TypeId, 3>{pad_type, site_type, turret_type}) {
        if (type == pad_type && !pad_targetable) continue;
        if (type != pad_type && pad_targetable) continue; // the pad case shoots the pad alone
        auto body = gun; body.type_id = type; body.weapons.clear();
        rules.combat.profiles.push_back(body);
        rules.combat.footprints.push_back({type, whole(2), whole(2), whole(2)});
    }
    return result;
}

struct BattleResult {
    Bytes state;
    std::uint64_t death{};
    bool built{}, pad_present{}, rejected{}, clean{};
    std::uint32_t owner{};
    std::uint64_t cooldown_until{};
};
BattleResult economy_battle(const EconomyCase& input, const sim::PartitionExecutor& executor, const bool restore) {
    auto session = take(TacticalSession::create(input.setup, input.content));
    must(session.submit({{0, 1, 1}, {5}, PadBuildPayload{site_type}}));
    const auto identities = checkpoint::tactical_identities(input.setup.content_identity);
    BattleResult result;
    bool started = false;
    for (std::uint64_t tick = 0; tick < 260; ++tick) {
        take(session.step(executor));
        if (const auto pad = session.pads().find(5); pad != session.pads().end()) {
            started = started || pad->second.under_construction != 0;
            result.built = result.built || pad->second.constructed != 0;
            if (started && result.death == 0 && pad->second.under_construction == 0 && pad->second.constructed == 0)
                result.death = tick;
        }
        // A save at every third frame: inside the build, while an enemy squad holds the site or the turret as its
        // target, on the frame a built object dies, and after the pad changed hands or died.
        if (restore && tick % 3 == 0) {
            const auto saved = checkpoint_bytes(session);
            auto resumed = take(checkpoint::load_tactical_checkpoint(saved, identities, factory(input.content), executor));
            expect(resumed.work.resimulated_ticks == 0 && checkpoint_bytes(resumed.session) == saved,
                "a land checkpoint taken during a pad battle restores exactly");
            session = std::move(resumed.session);
        }
    }
    result.pad_present = session.pads().contains(5);
    if (result.pad_present) {
        result.owner = session.unit_state(5)->owner;
        result.cooldown_until = session.pads().at(5).cooldown_until;
        // The player cannot build again on the pad it lost: the build is rejected and nothing is created.
        must(session.submit({{session.completed_tick(), 1, 2}, {5}, PadBuildPayload{site_type}}));
        const auto again = take(session.step(executor));
        result.rejected = count_events(again, EventKind::order_rejected) == 1 && session.construction().empty();
    }
    // units() returns a vector by value: take one copy so begin and end belong to the same vector.
    const auto survivors = session.units();
    result.clean = session.construction().empty() && std::none_of(survivors.begin(), survivors.end(),
        [](const UnitState& unit) { return unit.type_id == site_type || unit.type_id == turret_type; });
    result.state = session.canonical_state_bytes();
    return result;
}

void battle_contracts() {
    auto field = make_field(std::vector<std::string>(40, std::string(48, '.')));
    const sim::InlineExecutor serial;
    // A turret killed on its pad: the pad goes to the neutral player and starts its rebuild cooldown (WBP-28).
    const auto turret = battle_case(field.terrain, 1, false);
    const auto killed = economy_battle(turret, serial, true);
    expect(killed.built && killed.death != 0 && killed.pad_present && killed.owner == 3 && killed.clean
        && killed.cooldown_until == killed.death + 15 && killed.rejected,
        "LC-55/56: a killed turret neutralizes its pad, starts the rebuild cooldown and leaves no object behind");
    // A site killed while it is being built.
    const auto site = battle_case(field.terrain, 4, false);
    const auto unfinished = economy_battle(site, serial, true);
    expect(!unfinished.built && unfinished.death != 0 && unfinished.pad_present && unfinished.owner == 3 && unfinished.clean
        && unfinished.cooldown_until == unfinished.death + 15 && unfinished.rejected,
        "LC-55/56: a killed construction site neutralizes its pad and starts the rebuild cooldown");
    // The pad itself killed while a site stands on it: the site goes with it.
    const auto pad = battle_case(field.terrain, 4, true);
    const auto destroyed = economy_battle(pad, serial, true);
    expect(!destroyed.pad_present, "LC-55: the enemy squad kills the pad");
    expect(destroyed.clean, "LC-55: a killed pad takes its construction site with it");
    for (const auto* input : {&turret, &site, &pad}) {
        const auto baseline = economy_battle(*input, serial, false);
        expect(economy_battle(*input, serial, true).state == baseline.state,
            "a land pad battle with a save every third frame ends in the state of the straight run");
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            platform::ThreadWorkerAdapter threads(workers);
            expect(economy_battle(*input, threads, false).state == baseline.state,
                "land pad battle state equals on 1/2/4/8 workers");
        }
    }
}

// WPR-33: a buy with a prerequisite reads the ownership census, which a restore does not carry. The same buys give the
// same state with and without a save between them.
void prerequisite_contracts() {
    auto field = make_field(std::vector<std::string>(40, std::string(48, '.')));
    auto input = economy_case(field.terrain, 15, 9, 15);
    BuildOption gated; gated.type = 51; gated.kind = BuildKind::upgrade; gated.queue = BuildQueue::upgrades;
    gated.price = whole(50); gated.build_frames = gated.ai_build_frames = 10; gated.available = true;
    gated.requirements.prerequisites = {pad_type};
    input.content.economy.menus[1].options.push_back(gated);
    UpgradeProfile profile; profile.type = 51; input.content.economy.upgrades.push_back(profile);
    const sim::InlineExecutor serial;
    const auto buy_twice = [&](const bool restore) {
        auto session = take(TacticalSession::create(input.setup, input.content));
        must(session.submit({{2, 1, 1}, {6}, BuyPayload{51}}));
        must(session.submit({{5, 1, 2}, {6}, BuyPayload{51}}));
        const auto identities = checkpoint::tactical_identities(input.setup.content_identity);
        for (std::uint64_t tick = 0; tick < 8; ++tick) {
            take(session.step(serial));
            if (restore && tick == 3) {
                auto resumed = take(checkpoint::load_tactical_checkpoint(checkpoint_bytes(session), identities,
                    factory(input.content), serial));
                session = std::move(resumed.session);
            }
        }
        expect(session.ledgers()[0].queues[1].size() == 2 && session.ledgers()[0].credits == whole(1000 - 50 - 50 + 8),
            "a buy whose prerequisite the player owns is accepted before and after a save");
        return session.canonical_state_bytes();
    };
    expect(buy_twice(false) == buy_twice(true), "the same buys give the same state with and without a save between them");
}

void economy_contracts() {
    auto field = make_field(std::vector<std::string>(40, std::string(48, '.')));
    const auto input = economy_case(field.terrain, 15, 9, 15);
    const sim::InlineExecutor serial;
    const auto baseline = economy_run(input, serial, false);
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(workers);
        const auto result = economy_run(input, threads, true);
        expect(result.state == baseline.state && result.checkpoint == baseline.checkpoint,
            "land economy state and checkpoints equal on 1/2/4/8 workers with restore boundaries");
        test_support::ReverseOrderExecutor reverse(workers);
        const auto reversed = economy_run(input, reverse, true);
        expect(reversed.state == baseline.state && reversed.checkpoint == baseline.checkpoint,
            "reverse partition execution preserves the land economy state");
    }
    // The routes the economy does not take on land stay refused, and a content without rows keeps the old behaviour.
    auto session = take(TacticalSession::create(input.setup, input.content));
    // LC-42..51 (#5894): the land command surface takes Reinforce; a content with no reinforcement rules refuses it by reason.
    expect(static_cast<bool>(session.submit({{0, 1, 9}, {}, ReinforcePayload{}})), "land reinforcement is a land command");
    bool refused = false;
    for (const auto& event : take(session.step(serial)).snapshot->events())
        refused = refused || (event.kind == EventKind::order_rejected && event.sequence == 9 && event.reason == RejectReason::cannot_produce);
    expect(refused, "a land content without reinforcement rules refuses Reinforce with cannot_produce");
    auto plain = take(TacticalSession::create(setup(), content(field.terrain)));
    expect(plain.ledgers().empty() && plain.pads().empty(), "a land content without economy rows has no ledgers or pads");
    battle_contracts();
    prerequisite_contracts();
}

// Chief request 11-06: one stock map, two forced stuck squads, run at workers 1 and 8 with the state hash compared every
// tick. Squad 0 has a member that can never move (WLPA-93: the leader is held by the lag cap, then released and walks on).
// Squad 2 is a hard body walking into an idle hard body that cannot be pushed, with its route lock refused (WLPA-92: the pair
// becomes unresolvable, re-decides after the retry count and searches again). The test fails if either guard never fires.
struct GuardOutcome {
    std::vector<std::string> hashes;
    bool released{}, advanced{}, unresolvable{};
    std::size_t searches{};
};
GuardOutcome stock_guard_run(const std::shared_ptr<const LandTerrain>& terrain, const TacticalContent& tables,
    const TacticalSetup& initial, const sim::PartitionExecutor& executor) {
    GuardOutcome out;
    auto session = take(TacticalSession::create(initial, tables));
    auto edited = session.land_movement()->checkpoint();
    expect(edited.squads.size() == 4, "stock guard case has four squads");
    LandMovingMember* stuck = nullptr;
    for (auto& member : edited.members) {
        if (member.squad == 0) stuck = &member;
    }
    if (stuck) stuck->profile.maximum_speed = Fixed{};
    for (const std::size_t index : {2U, 3U}) edited.squads[index].collision_class = units::LandCollisionGroup::vehicle;
    edited.squads[3].maximum_speed = Fixed{}; // an immobile blocker cannot be pushed aside (WLPA-57)
    *const_cast<LandMovement*>(session.land_movement()) = LandMovement::from_checkpoint(edited).value();
    for (const std::size_t index : {0U, 2U}) {
        const auto& unit = initial.units[index];
        must(session.submit({{0, unit.owner, unit.entity_id}, {unit.entity_id},
            MovePayload{{land_cell_centre(terrain->width() * 2 / 3, terrain->spacing()), unit.position.y, {}}}}));
    }
    Fixed release_x;
    std::uint64_t charged_before = 0;
    bool locked = false;
    for (int tick = 0; tick < 240; ++tick) {
        out.hashes.push_back(take(session.step(executor)).state_sha256);
        const auto* movement = session.land_movement();
        if (!locked && movement->squads()[2].order == LandMoveOrder::following) {
            // The route lock of another peer (as the contact tests set it): every installation of a route is refused. It is
            // set once the order has been installed, because a new order releases the locks of its squad.
            auto locks = movement->checkpoint();
            for (auto& member : locks.members) if (member.squad == 2 || member.squad == 3) member.collision_lock = 99;
            *const_cast<LandMovement*>(session.land_movement()) = LandMovement::from_checkpoint(locks).value();
            locked = true;
        }
        const auto& leader = movement->squads()[0];
        if (!out.released && leader.straggler_wait_remaining == 0) { out.released = true; release_x = leader.position.x; }
        if (out.released && leader.position.x.raw() > release_x.raw() + whole(40).raw()) out.advanced = true;
        std::uint64_t charged = 0;
        for (const auto& contact : movement->contacts()) {
            charged += contact.charged_nodes;
            out.unresolvable |= contact.state == LandContactState::unresolvable;
        }
        if (charged > charged_before) ++out.searches;
        charged_before = charged;
    }
    expect(locked, "stock guard case: the pair's mover started its order");
    return out;
}

void stock_guard_case(const std::shared_ptr<const LandTerrain>& terrain, const TacticalContent& base, const TacticalSetup& setup_in) {
    auto tables = base;
    tables.land.contacts.complexity_quota = 100000000; // the stock graph is larger than the fixture quota: admit the searches
    auto initial = setup_in;
    // Unit 4 stands 60 units east of unit 3 on its row: the idle blocker of the pair.
    initial.units[3].position.y = initial.units[2].position.y;
    initial.units[3].position.x = Fixed::from_raw(initial.units[2].position.x.raw() + whole(60).raw());
    platform::ThreadWorkerAdapter one(1), eight(8);
    const auto first = stock_guard_run(terrain, tables, initial, one);
    const auto second = stock_guard_run(terrain, tables, initial, eight);
    std::size_t mismatch = 0;
    while (mismatch < first.hashes.size() && mismatch < second.hashes.size() && first.hashes[mismatch] == second.hashes[mismatch]) ++mismatch;
    const bool equal = first.hashes.size() == second.hashes.size() && mismatch == first.hashes.size();
    if (equal) std::cout << "stock guard case workers 1 vs 8: state hash matches on all " << first.hashes.size() << " ticks";
    else std::cout << "stock guard case workers 1 vs 8: first mismatching tick=" << mismatch;
    std::cout << " straggler_released=" << first.released << " leader_advanced=" << first.advanced
        << " pair_unresolvable=" << first.unresolvable << " contact_searches=" << first.searches << '\n';
    expect(equal, "stock guard case: the state hash is equal at workers 1 and 8 on every tick");
    expect(first.released && first.advanced,
        "stock guard case: the straggler guard (WLPA-93) released the held leader and the leader walked on");
    expect(first.unresolvable && first.searches >= 2,
        "stock guard case: the unresolvable pair (WLPA-92) re-decided and searched again after the retry count");
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
    // LC-54..57: the same pad, build, queue and income sequence on the stock map's decoded terrain. The pad rows are
    // authored synthetic profiles (the land loader reads no pad types yet); the field, the cells and the squads are stock.
    {
        auto input = economy_case(terrain, terrain->width() * 2 / 3, terrain->height() / 3, terrain->height() / 3 + 8);
        for (std::size_t i = 0; i < 4; ++i) {
            input.setup.units[i].position.x = land_cell_centre(terrain->width() / 3, terrain->spacing());
            input.setup.units[i].position.y = land_cell_centre(terrain->height() / 3 + static_cast<std::uint32_t>(i) * 3, terrain->spacing());
        }
        const auto economy_baseline = economy_run(input, serial, false);
        for (const std::size_t workers : {1U, 4U, 8U}) {
            platform::ThreadWorkerAdapter executor(workers);
            const auto result = economy_run(input, executor, true);
            expect(result.state == economy_baseline.state && result.checkpoint == economy_baseline.checkpoint,
                "stock land terrain economy session is equal across workers with restore boundaries");
        }
    }
    stock_guard_case(terrain, tables, initial);
    std::cout << "stock land session map=data/art/maps/_mp_land_naboo.ted width=" << terrain->width() << " height=" << terrain->height() << '\n';
    return checks.finish();
}
// LC-85..87: a land content with a fog grid and sensor rows fogs the session. The grid covers the terrain, the members
// reveal circles of their authored range, and the state (cells, anchors, checkpoint) is equal for every worker count.
TacticalContent fogged(const std::shared_ptr<const LandTerrain>& terrain, const std::int64_t range) {
    auto result = content(terrain);
    result.fog = take(skirmish::land_fog_rules(terrain->width(), terrain->height(), terrain->spacing(), whole(15), whole(6)));
    SensorProfile sensor;
    sensor.type_id = 12; sensor.reveal_range = whole(range);
    result.sensors.push_back(sensor);
    return result;
}

std::uint64_t instance_mask(const TacticalSession& session, const sim::EntityId id) {
    for (const auto& instance : session.snapshot()->instances())
        if (instance.entity_id == id) return instance.visible_to;
    throw std::runtime_error("land fog test: no published instance for the entity");
}

// The mask an enemy squad's members have for player 1 (bit 0) when it stands `distance` units from player 1's squad.
struct SeenResult { std::size_t seen{}, total{}; std::size_t revealed_cells{}; };
SeenResult seen_enemy_members(const std::shared_ptr<const LandTerrain>& terrain, const std::int64_t distance,
    const std::int64_t range) {
    auto input = setup();
    input.units.resize(2);
    input.units[0].position = {whole(100), whole(100), {}};
    input.units[1].owner = 2;
    input.units[1].position = {whole(100 + distance), whole(100), {}};
    const auto rules = fogged(terrain, range);
    auto session = take(TacticalSession::create(input, rules));
    SeenResult result;
    const auto members = session.land_movement()->members();
    const auto squads = session.land_movement()->squads();
    for (const auto& member : members) {
        if (squads[member.squad].object != 2) continue; // player 2's squad is the second root
        ++result.total;
        if ((instance_mask(session, member.object) & 1U) != 0U) ++result.seen;
    }
    const auto values = session.fog_cells()->values(0);
    result.revealed_cells = static_cast<std::size_t>(std::count_if(values.begin(), values.end(), [](const auto value) { return value != 0; }));
    return result;
}

void fog_contracts() {
    auto field = make_field(std::vector<std::string>(48, std::string(48, '.')));
    // 48 x 48 samples of 20: 960 units over 15 divides exactly into 64 square cells of 15.
    const auto rules = fogged(field.terrain, 130);
    expect(rules.fog && rules.fog->cells_wide == 64 && rules.fog->cells_tall == 64 && rules.fog->cell_size == whole(15)
            && rules.fog->map_left == Fixed{} && rules.fog->map_top == whole(960),
        "the land fog grid spans the terrain from its low corner, row 0 at the top");
    const auto near_enemy = seen_enemy_members(field.terrain, 90, 130);
    const auto far_enemy = seen_enemy_members(field.terrain, 600, 130);
    expect(near_enemy.total >= 1 && near_enemy.seen == near_enemy.total,
        "enemy members inside a member's authored reveal circle are visible to its player");
    expect(far_enemy.total == near_enemy.total && far_enemy.seen == 0,
        "enemy members outside every reveal circle are fogged");
    expect(near_enemy.revealed_cells > 100 && near_enemy.revealed_cells < 64 * 64 / 2 && far_enemy.revealed_cells > 100,
        "the first tick marks a bounded circle per revealer in the player's grid");
    // A range of 0 still reveals the floor of 10 units, which is one cell; it never reveals the enemy 90 away.
    const auto floored = seen_enemy_members(field.terrain, 90, 0);
    expect(floored.seen == 0 && floored.revealed_cells > 0 && floored.revealed_cells < near_enemy.revealed_cells,
        "a reveal range below the floor still reveals a floor-sized circle");
    // No fog grid: the same session without rules keeps the previous unfogged land session.
    auto plain = content(field.terrain);
    auto without = take(TacticalSession::create(setup(), plain));
    expect(without.fog_cells() == nullptr, "a land content without fog rules has no fog cells");

    const auto fog_tables = fogged(field.terrain, 130);
    const auto initial = setup();
    const auto baseline = run(initial, fog_tables, sim::InlineExecutor{}, false);
    const auto plain_run = run(initial, content(field.terrain), sim::InlineExecutor{}, false);
    expect(baseline.state != plain_run.state, "land fog changes the canonical state hash");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(workers);
        const auto result = run(initial, fog_tables, threads, true);
        expect(result.state == baseline.state && result.checkpoint == baseline.checkpoint,
            "land fog state and checkpoints equal on 1/2/4/8 workers with restore boundaries");
        test_support::ReverseOrderExecutor reverse(workers);
        const auto reversed = run(initial, fog_tables, reverse, true);
        expect(reversed.state == baseline.state && reversed.checkpoint == baseline.checkpoint,
            "reverse partition execution preserves land fog state and checkpoints");
    }
    // The revealers move with their squads: the last grid is not the first one.
    auto moving = take(TacticalSession::create(initial, fog_tables));
    commands(moving);
    const auto first_cells = std::vector<std::uint8_t>(moving.fog_cells()->values(0).begin(), moving.fog_cells()->values(0).end());
    sim::InlineExecutor serial;
    for (int tick = 0; tick < 200; ++tick) take(moving.step(serial));
    const auto later = moving.fog_cells()->values(0);
    expect(std::vector<std::uint8_t>(later.begin(), later.end()) != first_cells, "a moving revealer changes its player's grid");
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") return installed();
        contracts(); live_contracts(); fog_contracts(); corner_formation_contracts(); reorder_contracts(); maze_reorder_contracts(); victory_contracts(); economy_contracts(); stress(false); stress(); return checks.finish();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
