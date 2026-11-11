#include "eawr/checkpoint/tactical.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "land_test_support.hpp"

#include <algorithm>
#include <iostream>
#include <optional>

// Land garrisons, PR 1 (walk rows LC-37..LC-41, G-01..G-09): admission, slot accounting, the fade and the containment, exit
// placement, a stationary host, host death, and the checkpoint, replay and worker-count contracts of the garrison records.
// PR 2 (LC-101..LC-103): the occupants of a structure fire at the garrisoned range, a transport's occupants do not, and an
// occupant is never a target.
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

constexpr TypeId squad_company = 11, squad_member = 12, bunker_type = 21, blocker_type = 22;
constexpr TypeId vehicle_company = 41, vehicle_member = 42, enemy_company = 61, enemy_member = 62;

units::LandGarrisonData entrant_rule(const std::uint32_t value, const char* category = "Infantry") {
    units::LandGarrisonData data;
    data.entrant = true; data.categories = {category}; data.value = value;
    return data;
}
units::LandGarrisonData host_rule(const std::uint32_t slots, const std::int64_t radius, const std::int64_t exit_distance,
    const bool hovering = false, const char* category = "Infantry") {
    units::LandGarrisonData data;
    data.host = true; data.hovering = hovering; data.categories = {category};
    data.slots = slots; data.radius = whole(radius); data.exit_distance = whole(exit_distance);
    return data;
}

LandCompanyBinding infantry_binding(const TypeId company, const TypeId member_type, const char* name, const std::size_t count) {
    LandCompanyBinding binding;
    binding.type = company; binding.company.id = name;
    binding.company.team_max_speed = whole(60); binding.company.max_squad_size = static_cast<std::uint32_t>(count);
    binding.company.team_collision_class = units::LandCollisionGroup::infantry;
    binding.effective_team_max_speed = whole(60);
    binding.formation_spacing = Fixed::from_ratio(1, 5).value();
    for (std::size_t i = 0; i < count; ++i) {
        units::LandCompanyMember member;
        member.type = "synthetic-member"; member.kind = units::LandObjectKind::infantry;
        member.create_team = true; member.movement_class = "walker"; member.max_speed = whole(80);
        member.locomotor = "synthetic-walking";
        binding.company.members.push_back(member);
        binding.members.push_back({member_type, {whole(80), whole(120), whole(150), whole(2), whole(1), false,
            units::LandCollisionGroup::infantry}, 0, {}, {}, {}});
    }
    return binding;
}

// One vehicle in a generic team: it is a garrison host that walks, so it has to stop before its occupants leave.
LandCompanyBinding vehicle_binding() {
    LandCompanyBinding binding;
    binding.type = vehicle_company; binding.company.id = "synthetic-transport";
    binding.company.max_squad_size = 1;
    binding.company.team_locomotor = "TEAM_LOCOMOTOR";
    units::LandCompanyMember member;
    member.type = "synthetic-transport-member"; member.kind = units::LandObjectKind::infantry;
    member.create_team = true; member.movement_class = "walker"; member.locomotor = "synthetic-walking";
    binding.company.members.push_back(member);
    LandMemberBinding physical;
    physical.type = vehicle_member;
    physical.locomotion = {whole(40), whole(120), whole(150), whole(2), whole(3), false, units::LandCollisionGroup::bike};
    physical.hard_x = whole(1); physical.hard_y = whole(2);
    binding.members.push_back(physical);
    return binding;
}

struct Fixture {
    Field field;
    TacticalContent tables;
    TacticalSetup initial;
    sim::EntityId host{}, enemy{}, blocker{};
};

void add_garrison_rows(TacticalContent& tables, std::vector<std::pair<TypeId, units::LandGarrisonData>> rows) {
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& row : rows) tables.land.garrisons.push_back({row.first, std::move(row.second)});
}

// `squads` infantry squads of player 1 stand in a column west of a static bunker; an optional armed enemy squad stands south of
// it, and an optional wall block lies at the bunker's exit spot.
Fixture bunker_fixture(const std::size_t squads, const units::LandGarrisonData& entrant, const units::LandGarrisonData& host,
    const bool armed, const bool block_exit = false) {
    Fixture fx{make_field(std::vector<std::string>(40, std::string(64, '.'))), {}, {}, 0, 0, 0};
    auto& tables = fx.tables;
    tables.domain = BattleDomain::land; tables.land_terrain = fx.field.terrain; tables.land.clear_bit = clear;
    tables.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), whole(1)};
    tables.land.companies.push_back(infantry_binding(squad_company, squad_member, "synthetic-company", 3));
    if (armed) tables.land.companies.push_back(infantry_binding(enemy_company, enemy_member, "synthetic-enemy", 3));
    fx.initial.domain = BattleDomain::land;
    fx.initial.seed = 49;
    fx.initial.content_identity[0] = 91;
    fx.initial.players = {{1, 1, 1, player_flag_commandable}, {2, 2, 2, player_flag_commandable}};
    sim::EntityId next = 1;
    for (std::size_t i = 0; i < squads; ++i) {
        UnitState unit;
        unit.entity_id = next++; unit.type_id = squad_company; unit.owner = 1;
        unit.position = {whole(100), whole(60 + 90 * static_cast<std::int64_t>(i)), {}};
        fx.initial.units.push_back(unit);
    }
    if (armed) {
        UnitState unit;
        unit.entity_id = fx.enemy = next++; unit.type_id = enemy_company; unit.owner = 2;
        unit.position = {whole(500), whole(700), {}};
        fx.initial.units.push_back(unit);
    }
    UnitState bunker;
    bunker.entity_id = fx.host = next++; bunker.type_id = bunker_type; bunker.owner = 1;
    bunker.position = {whole(500), whole(300), {}};
    fx.initial.units.push_back(bunker);
    tables.land.static_objects.push_back(bunker_type);
    LandOccupation body; body.object = bunker.entity_id;
    tables.land.occupations.push_back(body);
    if (block_exit) {
        UnitState wall_unit;
        wall_unit.entity_id = fx.blocker = next++; wall_unit.type_id = blocker_type; wall_unit.owner = 1;
        wall_unit.position = {whole(540), whole(300), {}};
        fx.initial.units.push_back(wall_unit);
        tables.land.static_objects.push_back(blocker_type);
        LandOccupation wall_body; wall_body.object = wall_unit.entity_id; wall_body.primary_passability = wall;
        for (std::uint32_t row = 14; row <= 16; ++row) for (std::uint32_t column = 26; column <= 28; ++column)
            wall_body.primary_cells.push_back(row * fx.field.columns + column);
        tables.land.occupations.push_back(wall_body);
    }
    add_garrison_rows(tables, {{squad_company, entrant}, {bunker_type, host}});
    if (armed) {
        DamageRules damage;
        damage.shield_recharge_frames = 30; damage.damage_types = 1; damage.armor_types = 1;
        damage.armor_mods = {whole(1)}; tables.durability.damage = damage;
        DurabilityProfile bunker_health;
        bunker_health.type_id = bunker_type; bunker_health.max_hull = whole(12); bunker_health.allow_diminishing_firepower = false;
        tables.durability.profiles.push_back(bunker_health);
        auto enemy_health = bunker_health; enemy_health.type_id = enemy_member; enemy_health.max_hull = whole(40);
        tables.durability.profiles.push_back(enemy_health);
        CombatProfile gun;
        gun.type_id = enemy_member; gun.max_attack_distance = whole(150); gun.living_projectile_collision = true;
        gun.collision = CollisionBox{{whole(-2), whole(-2), whole(-2)}, {whole(2), whole(2), whole(4)}};
        WeaponProfile weapon;
        weapon.range = whole(150); weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 100;
        weapon.pulse_count = 1;
        ShotProfile shot; shot.damage = whole(4); shot.speed = whole(4); shot.max_travel = whole(500);
        weapon.shot = shot; gun.weapons.push_back(weapon);
        auto body_profile = gun; body_profile.type_id = bunker_type; body_profile.weapons.clear();
        tables.combat.profiles.push_back(body_profile);
        tables.combat.profiles.push_back(gun);
        // The bunker's footprint extent is 3 by 4: the length 5 is the exit distance a host without an authored one uses.
        tables.combat.footprints.push_back({bunker_type, whole(2), whole(3), whole(4)});
        tables.combat.footprints.push_back({enemy_member, whole(2), whole(2), whole(2)});
    }
    return fx;
}

Bytes checkpoint_bytes(const TacticalSession& session) {
    return take(checkpoint::save_tactical_checkpoint(session, session.pending_commands(),
        checkpoint::default_seats(session.record().setup), checkpoint::tactical_identities(session.record().setup.content_identity)));
}
checkpoint::TacticalSessionFactory factory(const TacticalContent& tables) {
    return [&tables](const TacticalReplay& replay) { return TacticalSession::from_replay(replay, tables); };
}
void roundtrip(TacticalSession& session, const Fixture& fx, const sim::PartitionExecutor& executor, const char* message) {
    const auto saved = checkpoint_bytes(session);
    auto loaded = take(checkpoint::load_tactical_checkpoint(saved,
        checkpoint::tactical_identities(fx.initial.content_identity), factory(fx.tables), executor));
    expect(checkpoint_bytes(loaded.session) == saved, message);
    session = std::move(loaded.session);
}

template <class Predicate>
bool run_until(TacticalSession& session, const sim::PartitionExecutor& executor, const int limit, Predicate&& done) {
    for (int i = 0; i < limit; ++i) {
        static_cast<void>(take(session.step(executor)));
        if (done()) return true;
    }
    return false;
}
bool live(const TacticalSession& session, const sim::EntityId id) { return session.unit_state(id).has_value(); }
std::size_t carried(const TacticalSession& session, const sim::EntityId host) {
    const auto state = session.unit_state(host);
    return state ? state->contained.size() : 0;
}
std::size_t hidden_count(const TacticalSession& session, const std::vector<sim::EntityId>& ids) {
    return static_cast<std::size_t>(std::count_if(ids.begin(), ids.end(), [&session](const sim::EntityId id) { return !live(session, id); }));
}
// Trigonometry and the extent length are fixed-point approximations: a position agrees when it is within a hundredth.
bool close(const sim::math::Vec2 a, const sim::math::Vec2 b) {
    const auto tolerance = Fixed::from_ratio(1, 100).value();
    const auto gap = [](const Fixed x, const Fixed y) { return x < y ? Fixed::from_raw(y.raw() - x.raw()) : Fixed::from_raw(x.raw() - y.raw()); };
    return gap(a.x, b.x) <= tolerance && gap(a.y, b.y) <= tolerance;
}
std::optional<LandMovingSquad> squad_of(const TacticalSession& session, const sim::EntityId id) {
    for (const auto& squad : session.land_movement()->squads()) if (squad.object == id) return squad;
    return std::nullopt;
}

// G-03: the cost is the container's value, charged per entering squad (never per company or per member), and pending entrants
// reserve nothing: the squad that arrives when the host is full is cleared and keeps standing where it is.
void capacity_contracts() {
    const sim::InlineExecutor serial;
    const std::vector<sim::EntityId> all{1, 2, 3, 4, 5, 6};
    {
        auto fx = bunker_fixture(6, entrant_rule(1), host_rule(5, 100, 40), false);
        auto session = take(TacticalSession::create(fx.initial, fx.tables));
        must(session.submit({{0, 1, 1}, all, LandGarrisonPayload{fx.host, 0}}));
        run_until(session, serial, 700, [] { return false; });
        expect(hidden_count(session, all) == 5, "G-03: a host with five slots takes five one-slot squads and refuses the sixth");
        expect(carried(session, fx.host) == 15, "WHE-06: every member of a contained squad is nested on the host");
        expect(session.land_movement()->squads().size() == 1 && session.land_movement()->members().size() == 3,
            "G-05: contained squads and members leave movement");
        roundtrip(session, fx, serial, "G-05: a full bunker restores byte for byte");
    }
    {
        auto fx = bunker_fixture(3, entrant_rule(2), host_rule(5, 100, 40), false);
        auto session = take(TacticalSession::create(fx.initial, fx.tables));
        must(session.submit({{0, 1, 1}, {1, 2, 3}, LandGarrisonPayload{fx.host, 0}}));
        run_until(session, serial, 700, [] { return false; });
        expect(hidden_count(session, {1, 2, 3}) == 2 && carried(session, fx.host) == 6,
            "G-03: a two-slot squad uses two slots; four of five are used and the third squad does not fit");
    }
    {
        // The last slot goes to the lower entrant id when two reach the fade on the same frame.
        auto fx = bunker_fixture(2, entrant_rule(1), host_rule(1, 100, 40), false);
        auto session = take(TacticalSession::create(fx.initial, fx.tables));
        must(session.submit({{0, 1, 1}, {1, 2}, LandGarrisonPayload{fx.host, 0}}));
        run_until(session, serial, 700, [] { return false; });
        expect(hidden_count(session, {1, 2}) == 1 && carried(session, fx.host) == 3,
            "G-04: two entrants and one slot leave exactly one inside");
    }
}

// G-02: an incompatible category is refused when the order is given, and nothing moves.
void category_contracts() {
    const sim::InlineExecutor serial;
    auto fx = bunker_fixture(1, entrant_rule(1), host_rule(5, 100, 40, false, "Vehicle"), false);
    auto session = take(TacticalSession::create(fx.initial, fx.tables));
    must(session.submit({{0, 1, 1}, {1}, LandGarrisonPayload{fx.host, 0}}));
    const auto first = take(session.step(serial));
    expect(first.diagnostics.size() == 1 && first.diagnostics.front().code == diagnostic_codes::command_rejected,
        "G-02: an infantry squad is refused by a vehicle-only host");
    const auto start = squad_of(session, 1)->position;
    run_until(session, serial, 120, [] { return false; });
    expect(live(session, 1) && carried(session, fx.host) == 0 && squad_of(session, 1)->position == start,
        "G-02: a refused squad stays where it stands");
    // Another player's squad and a unit that is not an entrant are refused as well.
    auto other = bunker_fixture(1, entrant_rule(1), host_rule(5, 100, 40), false);
    auto foreign = take(TacticalSession::create(other.initial, other.tables));
    must(foreign.submit({{0, 2, 1}, {1}, LandGarrisonPayload{other.host, 0}}));
    const auto refused = take(foreign.step(serial));
    expect(refused.diagnostics.size() == 1, "G-02: a player cannot garrison a squad or host it does not own");
}

// G-07 / LG-EXITSPOT: the exit spot is the start (the host centre) plus the exit distance along the host heading, and the
// occupant waits while that spot is not open ground.
void exit_contracts() {
    const sim::InlineExecutor serial;
    {
        auto fx = bunker_fixture(1, entrant_rule(1), host_rule(5, 100, 40), false);
        auto session = take(TacticalSession::create(fx.initial, fx.tables));
        must(session.submit({{0, 1, 1}, {1}, LandGarrisonPayload{fx.host, 0}}));
        expect(run_until(session, serial, 700, [&] { return !live(session, 1); }), "G-05: the squad walks in and disappears");
        roundtrip(session, fx, serial, "G-05: a squad inside restores byte for byte");
        must(session.submit({{session.completed_tick(), 1, 2}, {fx.host}, LandUnloadPayload{0, true}}));
        expect(run_until(session, serial, 30, [&] { return live(session, 1); }), "G-07: unload brings the squad out");
        const auto out = squad_of(session, 1);
        expect(out && close(out->position, at(540, 300)), "G-07: the exit spot is the host centre plus the exit distance");
        expect(carried(session, fx.host) == 0 && session.land_movement()->members().size() == 3,
            "G-07: the members leave the host and rejoin movement");
    }
    {
        auto fx = bunker_fixture(1, entrant_rule(1), host_rule(5, 100, 40), false, true);
        auto session = take(TacticalSession::create(fx.initial, fx.tables));
        must(session.submit({{0, 1, 1}, {1}, LandGarrisonPayload{fx.host, 0}}));
        expect(run_until(session, serial, 700, [&] { return !live(session, 1); }), "G-05: the squad walks in beside a blocked exit");
        must(session.submit({{session.completed_tick(), 1, 2}, {fx.host}, LandUnloadPayload{0, true}}));
        run_until(session, serial, 90, [] { return false; });
        expect(!live(session, 1) && carried(session, fx.host) == 3, "G-07: a blocked exit keeps the occupant waiting inside");
        roundtrip(session, fx, serial, "G-07: an occupant waiting to exit restores byte for byte");
        run_until(session, serial, 30, [] { return false; });
        expect(!live(session, 1), "G-07: the occupant is still waiting after the restore");
    }
    {
        // Two occupants leaving in one tick never share a spot.
        auto fx = bunker_fixture(2, entrant_rule(1), host_rule(5, 100, 40), false);
        auto session = take(TacticalSession::create(fx.initial, fx.tables));
        must(session.submit({{0, 1, 1}, {1, 2}, LandGarrisonPayload{fx.host, 0}}));
        expect(run_until(session, serial, 700, [&] { return !live(session, 1) && !live(session, 2); }), "both squads enter");
        must(session.submit({{session.completed_tick(), 1, 2}, {fx.host}, LandUnloadPayload{0, true}}));
        expect(run_until(session, serial, 30, [&] { return live(session, 1) && live(session, 2); }), "both squads leave together");
        const auto first = squad_of(session, 1), second = squad_of(session, 2);
        expect(first && second && !close(first->position, second->position), "simultaneous exits take different spots");
    }
    {
        // Without an authored exit distance the exit runs the length of the host's footprint extent (3 by 4: five).
        auto fx = bunker_fixture(1, entrant_rule(1), host_rule(5, 100, 0), true);
        auto session = take(TacticalSession::create(fx.initial, fx.tables));
        must(session.submit({{0, 1, 1}, {1}, LandGarrisonPayload{fx.host, 0}}));
        expect(run_until(session, serial, 700, [&] { return !live(session, 1); }), "G-05: the squad enters an armed bunker");
        must(session.submit({{session.completed_tick(), 1, 2}, {fx.host}, LandUnloadPayload{1, false}}));
        expect(run_until(session, serial, 30, [&] { return live(session, 1); }), "G-07: a single occupant can be ordered out");
        const auto out = squad_of(session, 1);
        expect(out && close(out->position, at(505, 300)), "G-07: an unauthored exit distance is the host extent length");
    }
}

// The fade lasts two frames: a restore at every frame of the walk-in catches it, and the digests must not change.
void fade_contracts() {
    const sim::InlineExecutor serial;
    auto fx = bunker_fixture(1, entrant_rule(1), host_rule(5, 100, 40), false);
    auto session = take(TacticalSession::create(fx.initial, fx.tables));
    must(session.submit({{0, 1, 1}, {1}, LandGarrisonPayload{fx.host, 0}}));
    for (int tick = 0; tick < 400 && live(session, 1); ++tick) {
        static_cast<void>(take(session.step(serial)));
        if (tick > 60) roundtrip(session, fx, serial, "G-05: a walking or fading entrant restores byte for byte");
    }
    expect(!live(session, 1), "G-05: the restored session still completes the entrance");
}

// G-04 / G-07: a walking host stops when something enters, and its occupants wait for it to stand still before they leave.
void transport_contracts() {
    const sim::InlineExecutor serial;
    Fixture fx{make_field(std::vector<std::string>(40, std::string(64, '.'))), {}, {}, 0, 0, 0};
    auto& tables = fx.tables;
    tables.domain = BattleDomain::land; tables.land_terrain = fx.field.terrain; tables.land.clear_bit = clear;
    tables.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), whole(1)};
    tables.land.companies.push_back(infantry_binding(squad_company, squad_member, "synthetic-company", 3));
    tables.land.companies.push_back(vehicle_binding());
    fx.initial.domain = BattleDomain::land; fx.initial.seed = 49; fx.initial.content_identity[0] = 92;
    fx.initial.players = {{1, 1, 1, player_flag_commandable}, {2, 2, 2, player_flag_commandable}};
    UnitState infantry; infantry.entity_id = 1; infantry.type_id = squad_company; infantry.owner = 1;
    infantry.position = {whole(100), whole(300), {}};
    UnitState transport; transport.entity_id = 2; transport.type_id = vehicle_company; transport.owner = 1;
    transport.position = {whole(500), whole(300), {}};
    fx.initial.units = {infantry, transport};
    // The transport row is keyed by the host's own type, the entrant row by the squad container's.
    add_garrison_rows(tables, {{squad_company, entrant_rule(1)}, {vehicle_member, host_rule(4, 100, 30)}});
    auto session = take(TacticalSession::create(fx.initial, tables));
    sim::EntityId host = 0;
    for (const auto& member : session.land_movement()->members())
        if (session.land_movement()->squads()[member.squad].object == 2) host = member.object;
    expect(host != 0, "the transport member exists");
    const auto state_of_host = [&]() -> std::optional<LandWalkState> {
        for (const auto& member : session.land_movement()->members()) if (member.object == host) return member.state;
        return std::nullopt;
    };
    must(session.submit({{0, 1, 1}, {2}, MovePayload{{whole(900), whole(300), {}}}}));
    must(session.submit({{10, 1, 2}, {1}, LandGarrisonPayload{host, 0}}));
    expect(run_until(session, serial, 900, [&] { return !live(session, 1); }), "G-05: the squad enters a walking transport");
    expect(carried(session, host) == 3, "WHE-06: the transport carries the squad's members");
    const auto stopped = state_of_host();
    expect(stopped && (*stopped == LandWalkState::stopped || *stopped == LandWalkState::deployed),
        "G-04: the transport stopped for the entrance");
    must(session.submit({{session.completed_tick(), 1, 3}, {2}, MovePayload{{whole(900), whole(300), {}}}}));
    for (int i = 0; i < 12; ++i) static_cast<void>(take(session.step(serial)));
    const auto walking = state_of_host();
    expect(walking && *walking != LandWalkState::stopped, "the transport is walking again");
    must(session.submit({{session.completed_tick(), 1, 4}, {host}, LandUnloadPayload{0, true}}));
    static_cast<void>(take(session.step(serial)));
    const auto braking = state_of_host();
    expect(braking && *braking != LandWalkState::stopped && !live(session, 1),
        "G-07: the occupant waits while the transport is still moving");
    expect(run_until(session, serial, 300, [&] { return live(session, 1); }), "G-07: the occupant leaves once the transport stood still");
    const auto standing = state_of_host();
    expect(standing && (*standing == LandWalkState::stopped || *standing == LandWalkState::deployed),
        "G-07: the occupant leaves only when the transport is stopped or deployed");
}

// G-09: a destroyed host that does not hover releases its occupants; a destroyed hovering host takes them down.
void death_contracts(const bool hovering) {
    const sim::InlineExecutor serial;
    auto fx = bunker_fixture(2, entrant_rule(1), host_rule(5, 100, 40, hovering), true);
    auto session = take(TacticalSession::create(fx.initial, fx.tables));
    must(session.submit({{0, 1, 1}, {1, 2}, LandGarrisonPayload{fx.host, 0}}));
    expect(run_until(session, serial, 900, [&] { return !live(session, 1) && !live(session, 2); }),
        "G-05: both squads enter the bunker before the attack");
    must(session.submit({{session.completed_tick(), 2, 1}, {fx.enemy}, MovePayload{{whole(500), whole(400), {}}}}));
    expect(run_until(session, serial, 1500, [&] { return !live(session, fx.host); }), "the armed enemy destroys the bunker");
    if (hovering) {
        expect(!live(session, 1) && !live(session, 2) && session.land_movement()->squads().size() == 1,
            "G-09: a hovering host takes its occupants down with it");
    } else {
        expect(live(session, 1) && live(session, 2) && session.land_movement()->squads().size() == 3,
            "G-09: a destroyed walking host or structure releases its occupants alive");
        roundtrip(session, fx, serial, "G-09: released occupants restore byte for byte");
    }
}

// Determinism: the scripted scenario enters, waits and exits; per-tick digests agree on 1/2/4/8 workers, with a restore at
// every few ticks, and the recorded replay reproduces the final state.
std::vector<std::string> scripted(const std::size_t workers, const bool restore, Bytes* final_state, Bytes* replay_state) {
    platform::ThreadWorkerAdapter executor(workers);
    auto fx = bunker_fixture(3, entrant_rule(1), host_rule(2, 100, 40), false);
    auto session = take(TacticalSession::create(fx.initial, fx.tables));
    must(session.submit({{0, 1, 1}, {1, 2, 3}, LandGarrisonPayload{fx.host, 0}}));
    must(session.submit({{420, 1, 2}, {fx.host}, LandUnloadPayload{0, true}}));
    std::vector<std::string> hashes;
    for (int tick = 0; tick < 560; ++tick) {
        hashes.push_back(take(session.step(executor)).state_sha256);
        if (restore && tick % 9 == 4) roundtrip(session, fx, executor, "scripted garrison checkpoint round trip");
    }
    if (final_state) *final_state = session.canonical_state_bytes();
    if (replay_state) {
        auto replay = take(parse_replay(take(write_replay(session.record()))));
        auto playback = take(TacticalSession::from_replay(replay, fx.tables));
        for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) static_cast<void>(take(playback.step(executor)));
        *replay_state = playback.canonical_state_bytes();
    }
    return hashes;
}
void determinism_contracts() {
    Bytes state, replayed;
    const auto reference = scripted(1, false, &state, &replayed);
    expect(replayed == state, "a recorded garrison and unload replay reproduces the final state");
    expect(scripted(1, true, nullptr, nullptr) == reference, "restoring garrison records mid-flight does not change any digest");
    for (const std::size_t workers : {2U, 4U, 8U})
        expect(scripted(workers, false, nullptr, nullptr) == reference, "garrison digests agree on 1/2/4/8 workers");
}


// ---- PR 2: occupant fire (LC-101..LC-103) -------------------------------------------------------------------------

CombatProfile gun_profile(const TypeId type, const std::int64_t range) {
    CombatProfile gun;
    gun.type_id = type; gun.max_attack_distance = whole(range); gun.living_projectile_collision = true;
    gun.collision = CollisionBox{{whole(-2), whole(-2), whole(-2)}, {whole(2), whole(2), whole(4)}};
    WeaponProfile weapon;
    weapon.range = whole(range); weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 100;
    weapon.pulse_count = 1;
    ShotProfile shot; shot.damage = whole(4); shot.speed = whole(4); shot.max_travel = whole(500);
    weapon.shot = shot; gun.weapons.push_back(weapon);
    return gun;
}

// The squad members get a gun of range 100 (the type of every garrisoning squad here) and the enemy a hull nothing here can
// empty, so the only way a shot reaches it is an occupant's.
void arm_squad_members(TacticalContent& tables, const std::optional<Fixed>& multiplier, const bool sturdy_bunker) {
    tables.land.garrisoned_range_multiplier = multiplier;
    tables.combat.profiles.insert(tables.combat.profiles.begin(), gun_profile(squad_member, 100));
    tables.combat.footprints.insert(tables.combat.footprints.begin(), {squad_member, whole(2), whole(2), whole(2)});
    for (auto& health : tables.durability.profiles) {
        if (health.type_id == enemy_member) health.max_hull = whole(100000);
        if (health.type_id == bunker_type && sturdy_bunker) health.max_hull = whole(100000);
    }
}

// One squad, a bunker at (500,300) and an enemy squad `gap` south of it that cannot reach the squad's walk-in.
Fixture fire_fixture(const std::int64_t gap, const std::optional<Fixed>& multiplier, const bool sturdy_bunker) {
    auto fx = bunker_fixture(1, entrant_rule(1), host_rule(5, 100, 40), true);
    arm_squad_members(fx.tables, multiplier, sturdy_bunker);
    for (auto& unit : fx.initial.units) if (unit.entity_id == fx.enemy) unit.position = {whole(500), whole(300 + gap), {}};
    return fx;
}

std::vector<sim::EntityId> members_of(const TacticalSession& session, const sim::EntityId squad_id) {
    std::vector<sim::EntityId> ids;
    const auto squads = session.land_movement()->squads();
    for (const auto& member : session.land_movement()->members())
        if (squads[member.squad].object == squad_id) ids.push_back(member.object);
    return ids;
}

struct FireLog {
    std::size_t inside{}, open{}, hits_on_occupants{}, at_host{};
};

// Steps `ticks` frames and counts the squad members' shots (while the squad is inside the host or standing in the open), the
// events that name a member as their target, and the events aimed at the host. Per-frame digests go to `digests`.
FireLog observe(TacticalSession& session, const sim::PartitionExecutor& executor, const sim::EntityId host,
    const std::vector<sim::EntityId>& members, const int ticks, std::vector<std::string>* digests = nullptr) {
    FireLog log;
    const auto is_member = [&members](const sim::EntityId id) {
        return std::find(members.begin(), members.end(), id) != members.end(); };
    for (int i = 0; i < ticks; ++i) {
        const auto output = take(session.step(executor));
        if (digests) digests->push_back(output.state_sha256);
        const bool inside = !live(session, 1);
        for (const auto& event : output.snapshot->combat_events()) {
            if (event.kind == CombatEventKind::weapon_fired && is_member(event.shooter)) ++(inside ? log.inside : log.open);
            if (event.target != 0 && is_member(event.target)) ++log.hits_on_occupants;
            if (event.target == host) ++log.at_host;
        }
    }
    return log;
}

// The squad walks into the bunker; returns its members' ids.
std::vector<sim::EntityId> enter_bunker(Fixture& fx, TacticalSession& session, const sim::PartitionExecutor& executor) {
    auto members = members_of(session, 1);
    must(session.submit({{0, 1, 1}, {1}, LandGarrisonPayload{fx.host, 0}}));
    expect(run_until(session, executor, 900, [&] { return !live(session, 1); }), "G-05: the squad walks into the bunker");
    return members;
}

// LC-101..LC-103 on a bunker: the occupants fire, farther by the data's multiplier, and nothing aims at them.
void bunker_fire_contracts() {
    const sim::InlineExecutor serial;
    const auto one_and_half = Fixed::from_ratio(3, 2).value();
    {
        // 130 away: past the squad's own range (100 and the enemy's size), inside 1.5 times it.
        auto fx = fire_fixture(130, one_and_half, true);
        auto session = take(TacticalSession::create(fx.initial, fx.tables));
        const auto members = enter_bunker(fx, session, serial);
        expect(members.size() == 3, "the test squad has three members");
        const auto log = observe(session, serial, fx.host, members, 300);
        expect(log.open == 0, "LC-101: no member is counted as standing in the open while the squad is inside");
        expect(log.inside > 0, "LC-101/LC-102: bunker occupants fire at 1.5 times their range");
        expect(log.hits_on_occupants == 0, "LC-103: an occupant is never a target of a shot or a hit");
        expect(log.at_host > 0, "LC-103: the enemy shoots, at the bunker and not at its occupants");
        roundtrip(session, fx, serial, "LC-101: a bunker with firing occupants restores byte for byte");
    }
    {
        auto fx = fire_fixture(130, std::nullopt, true);
        auto session = take(TacticalSession::create(fx.initial, fx.tables));
        const auto members = enter_bunker(fx, session, serial);
        const auto log = observe(session, serial, fx.host, members, 300);
        expect(log.inside == 0 && log.hits_on_occupants == 0,
            "LC-102: without the constant the same enemy is out of the occupants' range");
    }
    {
        // 80 away is inside the ordinary range: the occupants fire with or without the constant.
        auto fx = fire_fixture(80, std::nullopt, true);
        auto session = take(TacticalSession::create(fx.initial, fx.tables));
        const auto members = enter_bunker(fx, session, serial);
        const auto log = observe(session, serial, fx.host, members, 300);
        expect(log.inside > 0, "LC-101: occupants of a structure keep their weapons at their ordinary range");
    }
}

// LC-101: a transport disables its occupants' weapons, even with an enemy well inside their range.
void transport_fire_contracts() {
    const sim::InlineExecutor serial;
    Fixture fx{make_field(std::vector<std::string>(40, std::string(64, '.'))), {}, {}, 0, 0, 0};
    auto& tables = fx.tables;
    tables.domain = BattleDomain::land; tables.land_terrain = fx.field.terrain; tables.land.clear_bit = clear;
    tables.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), whole(1)};
    tables.land.companies.push_back(infantry_binding(squad_company, squad_member, "synthetic-company", 3));
    tables.land.companies.push_back(vehicle_binding());
    tables.land.companies.push_back(infantry_binding(enemy_company, enemy_member, "synthetic-enemy", 3));
    fx.initial.domain = BattleDomain::land; fx.initial.seed = 49; fx.initial.content_identity[0] = 93;
    fx.initial.players = {{1, 1, 1, player_flag_commandable}, {2, 2, 2, player_flag_commandable}};
    UnitState infantry; infantry.entity_id = 1; infantry.type_id = squad_company; infantry.owner = 1;
    infantry.position = {whole(100), whole(300), {}};
    UnitState transport; transport.entity_id = 2; transport.type_id = vehicle_company; transport.owner = 1;
    transport.position = {whole(500), whole(300), {}};
    UnitState enemy; enemy.entity_id = 3; enemy.type_id = enemy_company; enemy.owner = 2;
    enemy.position = {whole(500), whole(380), {}};
    fx.initial.units = {infantry, transport, enemy};
    add_garrison_rows(tables, {{squad_company, entrant_rule(1)}, {vehicle_member, host_rule(4, 100, 30)}});
    DamageRules damage;
    damage.shield_recharge_frames = 30; damage.damage_types = 1; damage.armor_types = 1;
    damage.armor_mods = {whole(1)}; tables.durability.damage = damage;
    DurabilityProfile enemy_health;
    enemy_health.type_id = enemy_member; enemy_health.max_hull = whole(100000); enemy_health.allow_diminishing_firepower = false;
    tables.durability.profiles.push_back(enemy_health);
    tables.combat.profiles.push_back(gun_profile(squad_member, 100));
    tables.combat.profiles.push_back(gun_profile(enemy_member, 150));
    tables.combat.footprints.push_back({squad_member, whole(2), whole(2), whole(2)});
    tables.combat.footprints.push_back({enemy_member, whole(2), whole(2), whole(2)});
    tables.land.garrisoned_range_multiplier = Fixed::from_ratio(3, 2).value();
    auto session = take(TacticalSession::create(fx.initial, tables));
    sim::EntityId host = 0;
    for (const auto& member : session.land_movement()->members())
        if (session.land_movement()->squads()[member.squad].object == 2) host = member.object;
    expect(host != 0, "the transport member exists");
    const auto members = members_of(session, 1);
    must(session.submit({{0, 1, 1}, {1}, LandGarrisonPayload{host, 0}}));
    expect(run_until(session, serial, 900, [&] { return !live(session, 1); }), "G-05: the squad enters the transport");
    const auto log = observe(session, serial, host, members, 300);
    expect(log.inside == 0 && log.hits_on_occupants == 0,
        "LC-101: a transport disables its occupants' weapons and an occupant is never a target");
}

// LC-101 / G-09: the bunker dies under fire, its occupants are released alive and fire from the open.
void release_fire_contracts() {
    const sim::InlineExecutor serial;
    auto fx = fire_fixture(400, Fixed::from_ratio(3, 2).value(), false);
    auto session = take(TacticalSession::create(fx.initial, fx.tables));
    const auto members = enter_bunker(fx, session, serial);
    must(session.submit({{session.completed_tick(), 2, 1}, {fx.enemy}, MovePayload{{whole(500), whole(380), {}}}}));
    FireLog total;
    for (int block = 0; block < 100 && live(session, fx.host); ++block) {
        const auto part = observe(session, serial, fx.host, members, 30);
        total.inside += part.inside; total.open += part.open; total.hits_on_occupants += part.hits_on_occupants;
    }
    expect(!live(session, fx.host) && live(session, 1), "G-09: the bunker is destroyed and its squad is released");
    expect(total.inside > 0, "LC-101: the occupants fired from inside while the enemy closed in");
    expect(total.hits_on_occupants == 0, "LC-103: the occupants were never hit while inside");
    const auto after = observe(session, serial, fx.host, members, 300);
    expect(after.open > 0, "LC-101: a released occupant fires like any squad in the open");
    expect(after.hits_on_occupants == 0, "the released squad has no hull of its own here, so nothing aims at it");
}

// Occupant fire is part of the digest: it agrees on 1/2/4/8 workers and does not change when the session is restored from a
// checkpoint at every few ticks.
std::vector<std::string> occupant_run(const std::size_t workers, const bool restore) {
    platform::ThreadWorkerAdapter executor(workers);
    auto fx = fire_fixture(130, Fixed::from_ratio(3, 2).value(), true);
    auto session = take(TacticalSession::create(fx.initial, fx.tables));
    must(session.submit({{0, 1, 1}, {1}, LandGarrisonPayload{fx.host, 0}}));
    std::vector<std::string> hashes;
    for (int tick = 0; tick < 700; ++tick) {
        hashes.push_back(take(session.step(executor)).state_sha256);
        if (restore && tick % 11 == 5) roundtrip(session, fx, executor, "occupant fire checkpoint round trip");
    }
    return hashes;
}
void occupant_determinism_contracts() {
    const auto reference = occupant_run(1, false);
    expect(occupant_run(1, true) == reference, "restoring a bunker with firing occupants does not change any digest");
    for (const std::size_t workers : {2U, 4U, 8U})
        expect(occupant_run(workers, false) == reference, "occupant fire digests agree on 1/2/4/8 workers");
}

} // namespace

int main() {
    try {
        capacity_contracts();
        category_contracts();
        exit_contracts();
        fade_contracts();
        transport_contracts();
        death_contracts(false);
        death_contracts(true);
        determinism_contracts();
        bunker_fire_contracts();
        transport_fire_contracts();
        release_fire_contracts();
        occupant_determinism_contracts();
        return checks.finish();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
