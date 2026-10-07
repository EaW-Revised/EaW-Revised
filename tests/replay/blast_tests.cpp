#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "../../src/sim/tactical/blast_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace t = eawr::sim::tactical;
namespace d = t::detail;
namespace m = eawr::sim::math;
using m::Fixed;
int failures{};
Fixed q(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }
m::Vec3 at(const std::int64_t x, const std::int64_t y = 0, const std::int64_t z = 0) { return {q(x), q(y), q(z)}; }
void expect(const bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

struct QueryFixture {
    std::vector<t::Player> players{{1, 1, 10, 1}, {2, 2, 20, 1}, {3, 1, 30, 1}, {4, 4, 40, 0}, {5, 5, 20, 1}};
    std::vector<t::SnapshotPlayer> relationships{{1, 1, false}, {2, 2, false}, {3, 1, false}, {4, 4, true}, {5, 5, false}};
    t::CombatTable table;
    t::CombatProfile profile;
    t::DurabilityProfile health;
    d::CollectionTrees trees;
    d::CombatWorld world;
    std::vector<d::CollectionTrees::Member> members;
    QueryFixture() {
        table.pad_neutral_factions = {40};
        profile.collision = t::CollisionBox{at(0), at(0)};
        world.players = players;
        world.relationships = relationships;
        world.table = &table;
        world.collection = &trees;
    }
    void add(const eawr::sim::EntityId id, const t::PlayerId owner, const m::Vec3 position) {
        d::CombatUnit unit;
        unit.id = id; unit.owner = owner; unit.position = position;
        unit.profile = &profile; unit.durability_profile = &health;
        unit.transform = m::to_matrix(m::identity_quat(), position).value();
        world.units.push_back(unit);
        const auto lo = profile.collision->min;
        const auto hi = profile.collision->max;
        const auto shifted = [&](const m::Vec3 value) {
            return m::Vec3{Fixed::from_raw(value.x.raw() + position.x.raw()),
                Fixed::from_raw(value.y.raw() + position.y.raw()), Fixed::from_raw(value.z.raw() + position.z.raw())};
        };
        members.push_back({id, owner, {shifted(lo), shifted(hi)}});
    }
    void build() {
        std::sort(world.units.begin(), world.units.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        trees.update(members, 0);
    }
};

t::Projectile payload() {
    t::Projectile projectile;
    projectile.owner = 1; projectile.shooter = 100;
    projectile.blast.damage = q(150); projectile.blast.radius = q(200);
    projectile.blast.dropoff = true; projectile.blast.max_delay = Fixed{};
    return projectile;
}

std::vector<d::BlastRecipient> recipients(const d::BlastStep& blast) {
    std::vector<d::BlastRecipient> result;
    for (const auto& player : blast.players) {
        result.insert(result.end(), player.recipients.begin(), player.recipients.end());
        if (player.capped) break;
    }
    return result;
}

void test_primary_and_cancellation() {
    auto projectile = payload();
    expect(d::primary_damage(projectile) == q(150), "WAD-02: zero primary uses type budget");
    projectile.damage = q(7);
    expect(d::primary_damage(projectile) == q(7), "WAD-02: positive instance wins");
    projectile.damage = q(-1);
    expect(d::primary_damage(projectile) == q(-1), "WAD-02: only exactly zero substitutes");
    projectile.damage = Fixed{};
    for (const auto field : {0, 1}) for (const auto nonpositive : {0, -1}) {
        auto disabled = projectile;
        (field == 0 ? disabled.blast.damage : disabled.blast.radius) = q(nonpositive);
        expect(d::primary_damage(disabled) == Fixed{}, "WAD-01/02: nonpositive budget/radius disables fallback");
    }
    t::HitOutcome hit;
    hit.shield_absorbed = true;
    expect(d::blast_after_hit(projectile, hit), "WAD-03: shield absorption keeps blast");
    hit.cancelled = true;
    expect(!d::blast_after_hit(projectile, hit), "WAD-03: successful cancellation suppresses blast");
}

void test_tiers() {
    auto projectile = payload();
    QueryFixture fixture;
    for (std::int64_t band = 0; band <= 5; ++band) fixture.add(static_cast<eawr::sim::EntityId>(band + 1), 2, at(40 * band));
    fixture.add(7, 2, at(201));
    fixture.add(8, 2, at(200, 0, 10000)); // planar centre branch admits altitude
    fixture.build();
    auto blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value()).size() == 7, "WAD-12: inclusive planar radius with unrestricted height");
    if (!blast) return;
    for (const auto& recipient : recipients(blast.value())) {
        const auto band = recipient.id == 8 ? 5 : static_cast<std::int64_t>(recipient.id - 1);
        expect(std::llabs(recipient.amount.raw() - 25 * (6 - band) * Fixed::scale) <= 150,
            "WAD-16: exact stepped Diamond Boron band");
    }
    for (std::int64_t band = 1; band <= 5; ++band) {
        auto below = d::blast_factor(projectile.blast, Fixed::from_raw(40 * band * Fixed::scale - 1), q(200));
        auto edge = d::blast_factor(projectile.blast, q(40 * band), q(200));
        expect(below && edge && below.value() > edge.value(), "WAD-16: just below a band never rounds up");
    }
    expect(d::blast_factor(projectile.blast, q(240), q(200)).value() == Fixed{}, "WAD-16: later admitted bands clamp to zero");
    projectile.blast.tiers = 1024;
    // Valid authored R=2^18 with two active x64 radius modes gives effective R=2^30.
    const auto enlarged = q(std::int64_t{1} << 30);
    const auto exact_edge = m::divide(q(1), q(1025)).value();
    const auto exact_below = m::divide(q(2), q(1025)).value();
    expect(d::blast_factor(projectile.blast, enlarged, enlarged).value() == exact_edge,
        "WAD-16: enlarged current radius retains exact outer tier without product overflow");
    expect(d::blast_factor(projectile.blast, Fixed::from_raw(enlarged.raw() - 1), enlarged).value() == exact_below,
        "WAD-16: enlarged radius preserves just-below tier boundary");
}

void test_relationships_cap_and_source() {
    QueryFixture fixture;
    for (t::PlayerId owner = 1; owner <= 5; ++owner) fixture.add(owner, owner, at(10));
    fixture.add(100, 1, at(-1000));
    fixture.build();
    auto projectile = payload(); projectile.blast.dropoff = false;
    auto blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value()).size() == 2, "WAD-08: own/allied/neutral owners excluded, same-faction enemy retained");
    expect(blast && blast.value().examined == 2, "WHZ-51/WAD-08: non-hostile players excluded before querying objects");
    fixture.relationships[0].neutral = true;
    blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && blast.value().examined == 0 && recipients(blast.value()).empty(),
        "WHZ-51/WAD-08: ordinary blast from a neutral owner has no hostile recipients");
    fixture.relationships[0].neutral = false;
    fixture.relationships[3].neutral = false;
    blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value()).size() == 3,
        "WHZ-51/WAD-08: captured station uses its current owner's hostile relationship");
    fixture.relationships[3].neutral = true;
    projectile.blast.immune_faction = 20;
    blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value()).size() == 3, "WAD-09: immunity excludes faction, admits shooter/allies/neutrals");
    projectile.blast.immune_faction.reset();
    projectile.blast.max_victims = 0;
    blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value()).size() == 1 && recipients(blast.value())[0].id == 2,
        "WAD-29: zero cap accepts first victim then stops all player queries");
    projectile.blast.max_victims = -3;
    blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value()).size() == 1, "WAD-29: negative cap has post-victim comparison");
    projectile.blast.max_victims = 2;
    blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value()).size() == 2, "WAD-29: cap resets for each player");

    QueryFixture source;
    source.add(1, 2, at(250)); source.add(100, 1, at(-1000)); source.build();
    projectile.blast.max_victims = 5000;
    source.world.units.back().scatter_radius = q(2);
    blast = d::prepare_blast(source.world, projectile, at(0));
    expect(blast && recipients(blast.value()).size() == 1 && recipients(blast.value())[0].amount == q(150),
        "WAD-10: current radius mode broadens query without scaling damage");
    source.world.units.pop_back();
    blast = d::prepare_blast(source.world, projectile, at(0));
    expect(blast && recipients(blast.value()).empty(), "WAD-10: deleted source restores unmodified radius");

    QueryFixture capped;
    for (eawr::sim::EntityId id = 1; id <= 6; ++id) capped.add(id, 2, at(static_cast<std::int64_t>(id * 10)));
    capped.build(); projectile.blast.max_victims = 1;
    blast = d::prepare_blast(capped.world, projectile, at(0));
    expect(blast && recipients(blast.value()).size() == 1 && blast.value().examined == 6,
        "WAD-13/29: cap does not retroactively limit the already copied query");
    const auto order = capped.trees.collect(2, {at(-200, -200, -200), at(200, 200, 200)});
    expect(blast && recipients(blast.value())[0].id == order.front(), "U-02: existing tree order retained");
    capped.health.hardpoints = {{t::HardpointRole::weapon, true, q(10)}};
    blast = d::prepare_blast(capped.world, projectile, at(0), order.front());
    expect(blast && recipients(blast.value()).empty() && blast.value().examined == 6,
        "WAD-22/29: object with no eligible hardpoint positions consumes the cap without a hull fallback");
    capped.health.hardpoints.clear();
    capped.world.units[0].in_limbo = true;
    projectile.blast.max_victims = 5000;
    blast = d::prepare_blast(capped.world, projectile, at(0));
    expect(blast && recipients(blast.value()).size() == 5, "WAD-11: limbo object never admitted");
}

void test_bounds_refinement_and_delay() {
    auto projectile = payload();
    QueryFixture fixture;
    fixture.profile.collision = t::CollisionBox{at(-60, -10, -10), at(60, 10, 10)};
    fixture.add(1, 2, at(250)); fixture.add(2, 2, at(250, 0, 1000)); fixture.build();
    auto blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value()).size() == 1, "WAD-12: centre outside radius uses 3D transformed bounds");
    expect(blast && std::llabs(recipients(blast.value())[0].amount.raw() - q(50).raw()) < 200,
        "WAD-15: hull contact refines planar falloff distance");
    projectile.blast.dropoff = false;
    projectile.blast.max_delay = q(1);
    blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value())[0].delay == Fixed::from_raw(Fixed::scale * 5 / 4),
        "WAD-17: no-dropoff delay uses centre distance even outside radius");
    QueryFixture tiny;
    tiny.add(1, 2, {Fixed::from_raw(Fixed::scale / 2), Fixed{}, Fixed{}}); tiny.build();
    blast = d::prepare_blast(tiny.world, projectile, at(0));
    expect(blast && recipients(blast.value())[0].delay == Fixed{}, "WAD-17: below one unit delay shortcut");
    tiny.profile.living_projectile_collision = false;
    blast = d::prepare_blast(tiny.world, projectile, at(0));
    expect(blast && recipients(blast.value()).empty(), "WAD-14: living-collision filter");
}

void test_hardpoint_selection_and_routing() {
    QueryFixture fixture;
    fixture.health.max_hull = q(1000);
    fixture.health.hardpoints = {{t::HardpointRole::weapon, true, q(100)},
        {t::HardpointRole::weapon, true, q(100)}, {t::HardpointRole::weapon, true, q(100)},
        {t::HardpointRole::other, false}, {t::HardpointRole::weapon, true, q(100)},
        {t::HardpointRole::weapon, true, q(100)}};
    fixture.profile.hardpoint_meshes = {"First", "Second", "Dead", "Dummy", "Boundary", "High"};
    // Deliberately shuffled input positions: delivery still follows HardPoints indices.
    fixture.profile.hardpoints = {{2, at(-100), false}, {1, at(-110), false}, {0, at(-120), true},
        {3, at(-120), true}, {4, at(80), true}, {5, at(-120, 0, 201), true}};
    fixture.add(1, 2, at(120)); fixture.build();
    auto state = t::full_durability(fixture.health); state.hardpoints[2] = Fixed{};
    fixture.world.units[0].durability = &state;
    auto projectile = payload();
    auto blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value()).size() == 3,
        "WAD-19: destroyed/untargetable shares included; non-destroyable, exact radius and high shares excluded");
    if (!blast) return;
    auto shares = recipients(blast.value());
    for (std::size_t i = 0; i < shares.size(); ++i) {
        expect(shares[i].hardpoint == i && shares[i].amount == q(25),
            "WAD-21: authored order and common object factor, not individual hardpoint falloff");
        auto hit = d::area_hit(projectile, shares[i], Fixed{});
        if (hit) expect(static_cast<bool>(t::apply_hit(fixture.health, {}, state, hit.value(), 0)),
            "WAD-23: secondary shares use ordinary damage");
    }
    expect(state.hull == q(1000) && state.hardpoints[0] == q(75) && state.hardpoints[1] == q(75)
        && state.hardpoints[2] == Fixed{}, "WAD-22: destroyed share discarded without redistribution or hull damage");
    blast = d::prepare_blast(fixture.world, projectile, at(0), 1, true, "fIrSt");
    shares = blast ? recipients(blast.value()) : std::vector<d::BlastRecipient>{};
    expect(shares.size() == 2 && shares[0].hardpoint == 1 && shares[1].hardpoint == 2
        && shares[0].amount == Fixed::from_raw(q(75).raw() / 2), "WAD-20: final direct mesh excluded case-insensitively before division");
    fixture.profile.hardpoint_meshes[1] = "FIRST";
    blast = d::prepare_blast(fixture.world, projectile, at(0), 1, true, "first");
    shares = blast ? recipients(blast.value()) : std::vector<d::BlastRecipient>{};
    expect(shares.size() == 1 && shares[0].hardpoint == 2 && shares[0].amount == q(75),
        "WAD-20: every matching direct mesh excluded, including duplicate authored selectors");
    fixture.profile.hardpoints = {{0, at(80), true}};
    blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value()).empty(), "WAD-22: no qualifying hardpoint never falls back to hull");

    fixture.profile.hardpoints = {{0, at(-120, 0, 200), false}};
    blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value()).empty(), "WAD-19: strict 3D vertical boundary");
    fixture.profile.hardpoints[0].position.z = Fixed::from_raw(q(200).raw() - 1);
    blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value()).size() == 1, "WAD-19: one raw unit below radius admitted without length rounding");
    fixture.profile.hardpoints[0].position = {Fixed::from_raw(-q(120).raw() + Fixed::scale / 2), Fixed{}, Fixed{}};
    projectile.blast.max_delay = q(1);
    blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value())[0].delay.raw() > 0, "WAD-21: sub-unit hardpoint distance has no hull delay shortcut");
    fixture.profile.hardpoints[0].position = at(-117, 0, 4);
    blast = d::prepare_blast(fixture.world, projectile, at(0));
    expect(blast && recipients(blast.value())[0].delay == m::divide(q(5), q(200)).value(),
        "WAD-21: delay uses spatial hardpoint distance, independently of centre falloff");

    projectile.target = 1; projectile.target_hardpoint = 2;
    projectile.damage = q(999); projectile.source_damage_factor = q(2);
    projectile.damage_type = 7; projectile.shield_damage = false; projectile.energy_damage = true;
    auto hit = d::area_hit(projectile, {1, q(25), {}, 1}, q(1));
    expect(hit && hit.value().area && hit.value().projectile && hit.value().hardpoint == 1
        && hit.value().amount == q(50) && hit.value().damage_type == 7 && !hit.value().shield_damage
        && hit.value().energy_damage && hit.value().hitpoint_damage && hit.value().defense == q(1),
        "WAD-23/24/25: retain type/flags/modifiers, ignore original aimed index and instance direct damage");
    d::ProjectileStep flight; flight.projectile = projectile; flight.hit = 1;
    flight.meshed = true; flight.mesh_hardpoint = 0;
    expect(d::direct_blast_mesh(fixture.world, flight) == "First", "WAD-20: exclusion uses flight's final routed mesh");
    fixture.profile.hardpoint_meshes[1] = "Second";
    fixture.profile.hardpoints = {{0, at(-120), true}, {1, at(-110), true}};
    fixture.world.units[0].transform = m::to_matrix({Fixed{}, Fixed{}, q(1), Fixed{}}, at(120)).value();
    blast = d::prepare_blast(fixture.world, payload(), at(0));
    expect(blast && recipients(blast.value()).empty(), "WAD-19: hardpoint distance uses recipient rotation and translation");
}

struct Battle {
    t::TacticalReplay replay;
    t::CombatTable combat;
    t::DurabilityTable health;
    t::AbilityTable abilities;
    std::vector<t::SensorProfile> sensors;
    Battle(const bool expiry, const bool delay) {
        replay.setup.seed = 1069;
        replay.setup.players = {{1, 1, 10, 1}, {2, 2, 20, 1}};
        replay.setup.units = {{1, 1, 1, at(0)}, {2, 2, 2, at(300)}, {3, 2, 2, at(280, 80)}};
        for (eawr::sim::EntityId id = 10; id < 210; ++id) replay.setup.units.push_back({id, 2, 2, at(10000 + static_cast<std::int64_t>(id * 10))});
        replay.commands = {{{0, 1, 0}, {1}, t::AttackPayload{2}}}; replay.final_tick_count = 70;
        t::CombatProfile shooter; shooter.type_id = 1; shooter.max_attack_distance = q(500);
        t::WeaponProfile weapon; weapon.range = q(500); weapon.hardpoint = t::object_weapon;
        weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 1000;
        weapon.pulse_count = 1;
        t::ShotProfile shot; shot.damage = q(7); shot.speed = q(25); shot.max_travel = q(280);
        shot.blast = payload().blast; shot.blast.dropoff = false;
        shot.blast.max_delay = delay ? q(1) : Fixed{}; weapon.shot = shot;
        shooter.weapons = {weapon};
        t::CombatProfile target; target.type_id = 2;
        if (!expiry) target.collision = t::CollisionBox{at(-20, -10, -10), at(20, 10, 10)};
        combat.profiles = {shooter, target};
        t::DurabilityProfile hp; hp.type_id = 1; hp.max_hull = q(1000);
        auto target_hp = hp; target_hp.type_id = 2;
        health.profiles = {hp, target_hp}; health.damage = t::DamageRules{};
        health.damage->shield_recharge_frames = 90; health.damage->energy_recharge_frames = 150;
        sensors = {{1, q(20000)}, {2, q(20000)}};
    }
    t::TacticalSession make() const {
        auto made = t::TacticalSession::from_replay(replay, sensors, health, {}, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(made), "blast battle creates"); return std::move(made).value();
    }
};

std::vector<std::string> trace(const Battle& battle, const eawr::sim::PartitionExecutor& executor,
    const bool explicit_explosion = false, const bool remove_source = false) {
    auto session = battle.make();
    std::vector<std::string> rows;
    std::uint64_t detonations{}, examined{};
    bool requested = false;
    bool removed = false;
    for (std::uint64_t tick = 0; tick < battle.replay.final_tick_count; ++tick) {
        if (explicit_explosion && !requested && !session.projectiles().empty()
            && session.projectiles()[0].position.x >= q(250)) {
            requested = session.request_projectile_explosion(session.projectiles()[0].id);
        }
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "blast battle step succeeds"); if (!stepped) break;
        detonations += stepped.value().blast_detonations; examined += stepped.value().blast_recipients_examined;
        if (remove_source && !removed && stepped.value().blast_detonations != 0) {
            auto submitted = session.submit({{tick + 1, 1, 1}, {1}, t::DamagePayload{q(1000)}});
            expect(static_cast<bool>(submitted), "U-04: source deletion queued after detonation before delayed delivery");
            removed = static_cast<bool>(submitted);
        }
        rows.push_back(stepped.value().state_sha256 + stepped.value().snapshot->sha256()
            + std::to_string(stepped.value().blast_detonations) + ":" + std::to_string(stepped.value().blast_recipients_examined));
        if (stepped.value().blast_detonations == 0) expect(stepped.value().blast_recipients_examined == 0,
            "work contract: no recipient query during ordinary flight");
    }
    expect(detonations == 1 && examined <= 3 && examined > 0, "work contract: one detonation ignores 200 distant objects");
    const auto primary = session.durability_state(2);
    const auto secondary = session.durability_state(3);
    expect(secondary && secondary->hull == q(850), "secondary budget uses ordinary hull path once");
    expect(primary && primary->hull == q(explicit_explosion || !battle.combat.profiles[1].collision ? 850 : 993),
        "primary exclusion applies only to a direct contact");
    if (explicit_explosion) expect(requested, "WAD-05: explicit service request accepted");
    if (remove_source) expect(removed && !session.durability_state(1), "U-04: delayed source deleted");
    return rows;
}

void test_session_determinism() {
    const eawr::sim::InlineExecutor inline_executor;
    for (const auto expiry : {false, true}) for (const auto delay : {false, true}) {
        const Battle battle(expiry, delay);
        const auto reference = trace(battle, inline_executor);
        for (std::size_t workers = 1; workers <= 8; workers *= 2) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(trace(battle, executor) == reference, "WAD: hashes, snapshots, journals and work identical with " + std::to_string(workers) + " workers");
        }
        auto bytes = t::write_replay(battle.replay);
        expect(static_cast<bool>(bytes), "blast replay writes");
        if (bytes) {
            auto parsed = t::parse_replay(bytes.value());
            expect(static_cast<bool>(parsed), "blast replay parses");
            if (parsed) { auto restored = battle; restored.replay = parsed.value(); expect(trace(restored, inline_executor) == reference, "blast replay round trip"); }
        }
    }
    const Battle explicit_battle(true, false);
    const auto reference = trace(explicit_battle, inline_executor, true);
    for (std::size_t workers = 1; workers <= 8; workers *= 2) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(trace(explicit_battle, executor, true) == reference, "explicit detonation worker equivalence");
    }
    Battle delayed(true, true);
    const auto retained = trace(delayed, inline_executor, false, true);
    for (std::size_t workers = 1; workers <= 8; workers *= 2) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(trace(delayed, executor, false, true) == retained, "U-04 policy: queued blast survives shooter deletion");
    }
    Battle absorbed(false, false);
    absorbed.health.profiles[1].max_shields = q(1000);
    auto session = absorbed.make();
    bool seen = false;
    for (std::uint64_t tick = 0; tick < 70; ++tick) {
        auto stepped = session.step(inline_executor);
        expect(static_cast<bool>(stepped), "WAD-03: absorption battle steps");
        if (!stepped) break;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            if (event.kind == t::CombatEventKind::projectile_hit) {
                seen = (event.outcome & t::hit_outcome_shield_absorbed) != 0;
                const auto secondary = session.durability_state(3);
                expect(secondary && secondary->hull == q(1000) && secondary->shields == q(850),
                    "WAD-03/24: absorbed primary still blasts secondary through ordinary shields");
            }
        }
    }
    expect(seen, "WAD-03: primary absorption observed");
}

void test_rocket_blast_determinism() {
    const eawr::sim::InlineExecutor inline_executor;
    for (const bool expiry : {false, true}) for (const bool delay : {false, true}) {
        Battle battle(expiry, delay);
        if (!expiry) battle.combat.profiles[1].collision = t::CollisionBox{at(-50, -10, -10), at(20, 10, 10)};
        auto& shot = *battle.combat.profiles[0].weapons[0].shot;
        t::FlightProfile flight; flight.kind = t::FlightKind::rocket; flight.target_radius = true;
        flight.authored_distance = q(5000); flight.curve_distance = q(500); flight.straight_distance = q(500);
        shot.flight = flight; shot.max_travel = q(5000);
        const auto reference = trace(battle, inline_executor);
        for (std::size_t workers = 1; workers <= 8; workers *= 2) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(trace(battle, executor) == reference,
                "WAD-04/RFL-06/08: one blast for impact or current-position path exhaustion, identical workers 1/2/4/8");
        }
    }
}

std::vector<std::string> hardpoint_trace(const bool shield_damage, const bool delayed,
    const bool meshed, const eawr::sim::PartitionExecutor& executor) {
    Battle battle(false, false);
    auto& target = battle.combat.profiles[1];
    target.hardpoints = {{0, at(0), true}, {1, at(20), false}, {2, at(40), false}};
    target.hardpoint_meshes = {"primary", "secondary", "last"};
    if (meshed) {
        // The collision triangle belongs to hardpoint 1, but the ordered aim routes to 0.
        target.meshes = {t::collision_mesh({{at(-20, -10, -10), at(-20, 10, -10), at(-20, 0, 10)}}, 1,
            t::no_hardpoint, false)};
        target.mesh_bounds = target.collision; target.aimed_routes = {0, 1, 2};
    }
    auto& health = battle.health.profiles[1];
    health.hardpoints = {{t::HardpointRole::weapon, true, q(1000)},
        {t::HardpointRole::weapon, true, q(1000)}, {t::HardpointRole::weapon, true, q(1000)}};
    health.max_shields = q(100);
    auto& shot = *battle.combat.profiles[0].weapons[0].shot;
    shot.shield_damage = shield_damage; shot.blast.max_delay = delayed ? q(1) : Fixed{};
    // WAD-30: capital-only targeting leaves the nearby individual craft in the blast query.
    battle.combat.profiles[0].weapons[0].category_restrictions = 1;
    battle.replay.setup.units[2].type_id = 3;
    auto craft = target; craft.type_id = 3; craft.category_bits = 1;
    craft.hardpoints.clear(); craft.hardpoint_meshes.clear(); craft.meshes.clear(); craft.aimed_routes.clear();
    battle.combat.profiles.push_back(craft);
    auto craft_health = health; craft_health.type_id = 3; craft_health.hardpoints.clear();
    battle.health.profiles.push_back(craft_health); battle.sensors.push_back({3, q(20000)});
    auto session = battle.make();
    std::vector<std::string> rows;
    std::uint64_t detonations{}, impacts{};
    for (std::uint64_t tick = 0; tick < 70; ++tick) {
        const auto step = session.step(executor);
        expect(static_cast<bool>(step), "WAD-21: hardpoint battle steps"); if (!step) break;
        detonations += step.value().blast_detonations;
        for (const auto& event : step.value().snapshot->combat_events())
            if (event.kind == t::CombatEventKind::projectile_hit) ++impacts;
        rows.push_back(step.value().state_sha256 + step.value().snapshot->sha256()
            + std::to_string(step.value().blast_detonations) + ":" + std::to_string(step.value().blast_recipients_examined));
    }
    const auto primary = session.durability_state(2);
    const auto secondary = session.durability_state(3);
    expect(detonations == 1 && impacts == 1, "WAD-03/28: direct and split area delivery emit one impact/detonation");
    expect(primary && primary->hardpoints[0] == q(shield_damage ? 1000 : 993)
        && primary->hardpoints[1] == q(shield_damage || delayed ? 1000 : 925)
        && primary->hardpoints[2] == q(delayed ? (shield_damage ? 943 : 950) : (shield_damage ? 943 : 925)),
        "WAD-20/21/23/24: exclude final direct route; split budget visits other meshes through ordinary shields");
    expect(primary && primary->shields == q(shield_damage || delayed ? 0 : 100),
        "WAD-24/26: immediate bypass retains shields; source-less delayed delivery uses ordinary shield routing");
    expect(secondary && secondary->hull == q(shield_damage || delayed ? 950 : 850)
        && secondary->shields == q(shield_damage || delayed ? 0 : 100),
        "WAD-18/30: nearby craft receives one ordinary delivery despite direct-target category restriction");
    return rows;
}

void test_hardpoint_session_determinism() {
    const eawr::sim::InlineExecutor inline_executor;
    for (const auto shield_damage : {false, true}) for (const auto delay : {false, true}) for (const auto meshed : {false, true}) {
        const auto reference = hardpoint_trace(shield_damage, delay, meshed, inline_executor);
        for (std::size_t workers = 1; workers <= 8; workers *= 2) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(hardpoint_trace(shield_damage, delay, meshed, executor) == reference,
                "WAD-21: immediate/delayed shares, hashes, snapshots and work match with " + std::to_string(workers) + " workers");
        }
    }
}

void test_shield_generator_split() {
    Battle battle(false, false);
    auto& target = battle.combat.profiles[1];
    target.hardpoints = {{0, at(0), true}, {1, at(20), false}, {2, at(40), false}};
    target.hardpoint_meshes = {"primary", "generator", "last"};
    auto& health = battle.health.profiles[1];
    health.max_shields = q(100);
    health.hardpoints = {{t::HardpointRole::weapon, true, q(1000)},
        {t::HardpointRole::shield_generator, true, q(25)}, {t::HardpointRole::weapon, true, q(1000)}};
    battle.combat.profiles[0].weapons[0].shot->shield_damage = false;
    auto session = battle.make();
    const eawr::sim::InlineExecutor executor;
    std::vector<std::uint32_t> lost;
    for (std::uint64_t tick = 0; tick < 70; ++tick) {
        const auto step = session.step(executor);
        expect(static_cast<bool>(step), "WAD-25: generator split battle steps"); if (!step) break;
        for (const auto& event : step.value().snapshot->events()) {
            if (event.kind == t::EventKind::hardpoint_destroyed && event.unit == 2) lost.push_back(event.hardpoint);
        }
    }
    const auto primary = session.durability_state(2);
    expect(primary && primary->hardpoints[0] == q(993) && primary->hardpoints[1] == Fixed{}
        && primary->hardpoints[2] == q(925) && primary->shields == Fixed{},
        "WAD-21/25: generator loss drops shield; overkill stays discarded and later shares keep their original amount");
    expect(lost == std::vector<std::uint32_t>{1}, "WAD-25: ordinary hardpoint death side effects run once for the selected share");
}

std::vector<std::string> source_death_trace(const Fixed radius_mode, const std::int32_t cap,
    const eawr::sim::PartitionExecutor& executor) {
    Battle battle(true, false);
    battle.replay.setup.units[1].position = at(radius_mode > q(1) ? 250 : 150);
    battle.replay.setup.units[2].position = at(50);
    battle.health.profiles[0].max_hull = q(100);
    auto& blast = battle.combat.profiles[0].weapons[0].shot->blast;
    blast.immune_faction = 30; blast.max_victims = cap;
    t::AbilityProfile mode; mode.kind = t::AbilityKind::turbo; mode.modifiers.scatter_radius = radius_mode;
    battle.abilities.profiles = {{1, {mode}, false}};
    battle.replay.commands = {{{0, 1, 0}, {1}, t::AbilityPayload{t::AbilityKind::turbo, t::AbilityAction::activate}},
        {{1, 1, 1}, {1}, t::AttackPayload{2}}};
    auto session = battle.make();
    bool requested = false;
    std::uint64_t detonations{}, examined{};
    std::vector<std::string> rows;
    for (std::uint64_t tick = 0; tick < battle.replay.final_tick_count; ++tick) {
        if (!requested && !session.projectiles().empty())
            requested = session.request_projectile_explosion(session.projectiles()[0].id);
        const auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "WAD-10: own-source blast steps");
        if (!stepped) break;
        detonations += stepped.value().blast_detonations; examined += stepped.value().blast_recipients_examined;
        rows.push_back(stepped.value().state_sha256 + stepped.value().snapshot->sha256()
            + std::to_string(stepped.value().blast_recipients_examined));
    }
    expect(requested && detonations == 1 && examined > 0 && examined <= 6,
        "WAD-10: two staged radii stay bounded despite 200 distant objects");
    expect(!session.durability_state(1), "WAD-10: earlier player group kills non-immune shooter");
    const auto later = session.durability_state(2);
    const auto inside = session.durability_state(3);
    expect(later && later->hull == q(cap == 1 || radius_mode > q(1) ? 1000 : 850),
        "WAD-10: later player uses restored radius after source dies, for enlarged or reduced modes");
    expect(inside && inside->hull == q(cap == 1 ? 1000 : 850),
        "WAD-29: selected player cap still exits the whole blast");
    return rows;
}

void test_source_death_between_players() {
    const eawr::sim::InlineExecutor inline_executor;
    for (const auto radius_mode : {q(2), Fixed::from_raw(Fixed::scale / 2)}) for (const auto cap : {1, 2, 5000}) {
        const auto reference = source_death_trace(radius_mode, cap, inline_executor);
        for (std::size_t workers = 1; workers <= 8; workers *= 2) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(source_death_trace(radius_mode, cap, executor) == reference,
                "WAD-10: per-player radius/cap choice is identical with " + std::to_string(workers) + " workers");
        }
    }
}

template <typename Profile>
void explicit_delay(Profile& profile, const Fixed delay) {
    if constexpr (requires { profile.damage_delay; }) profile.damage_delay = delay;
}

template <typename Profile>
void death_payloads(Profile& profile, const std::vector<t::ShotProfile>& shots, const Fixed height) {
    if constexpr (requires { profile.death_projectiles; profile.ranged_target_z_adjust; }) {
        profile.death_projectiles = shots;
        profile.ranged_target_z_adjust = height;
    }
}

std::vector<std::string> death_payload_trace(const bool empty, const bool chain,
    const eawr::sim::PartitionExecutor& executor) {
    Battle battle(false, false);
    battle.replay.setup.players = {{1, 1, 10, 1}, {2, 1, 20, 1}, {4, 4, 40, 0}};
    battle.replay.setup.units = {{1, 1, 4, at(0)}, {2, 2, 1, at(50)}, {3, 2, 2, at(60)}};
    if (chain) battle.replay.setup.units.push_back({4, 1, 4, at(100)});
    battle.replay.setup.units.front().rotation = {{}, {}, q(1), {}};
    battle.combat.profiles[0].weapons.clear();
    battle.combat.profiles[0].collision = t::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
    battle.combat.pad_neutral_factions = {40};
    battle.health.profiles[0].max_hull = q(20);
    battle.health.profiles[1].max_hull = q(10000);
    t::ShotProfile payload;
    payload.speed = q(6); payload.blast.damage = q(3000); payload.blast.radius = q(400);
    payload.blast.max_delay = {}; payload.blast.immune_faction = 20;
    t::FlightProfile flight; flight.lifetime = Fixed{}; payload.flight = flight;
    explicit_delay(payload, Fixed::from_raw(Fixed::scale / 2));
    death_payloads(battle.combat.profiles[0], empty ? std::vector<t::ShotProfile>{}
        : std::vector<t::ShotProfile>{payload}, q(30));
    battle.replay.commands = {{{0, 1, 0}, {1}, t::DamagePayload{q(20)}}};
    auto session = battle.make();
    std::vector<std::string> rows;
    std::uint64_t detonations{};
    std::uint64_t damaged_frame{};
    for (std::uint64_t frame = 0; frame < 40; ++frame) {
        const auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "WNO-29: container-death session steps");
        if (!stepped) break;
        detonations += stepped.value().blast_detonations;
        if (frame == 0) {
            expect(!session.durability_state(1), "WCC-72: dead source is removed before detonation");
            expect(session.projectiles().size() == (empty ? 0U : 1U), "WNO-29: empty/list spawns zero/one payload");
            if (!session.projectiles().empty()) {
                const auto& shot = session.projectiles().front();
                expect(shot.owner == 4 && shot.shooter == 1 && shot.position == at(0, 0, 30)
                    && shot.step.x == q(-6) && shot.step.y == Fixed{},
                    "WNO-29/30: dead owner/source/facing/height retained rather than killer owner");
            }
        }
        const auto recipient = session.durability_state(2);
        const auto rebel = session.durability_state(3);
        expect(rebel && rebel->hull == q(10000), "WNO-31: allied Rebel remains faction-immune");
        if (recipient && recipient->hull < q(10000) && damaged_frame == 0) damaged_frame = frame;
        if (frame < 4) expect(recipient && recipient->hull == q(10000),
            "WNO-32: explicit delay prevents immediate recipient damage");
        rows.push_back(stepped.value().state_sha256 + stepped.value().snapshot->sha256());
    }
    const auto recipient = session.durability_state(2);
    expect(detonations == (empty ? 0U : chain ? 2U : 1U), "WNO-29/31: one detonation per death including Hutt chain");
    expect(recipient && recipient->hull == q(empty ? 10000 : chain ? 4000 : 7000),
        "WNO-31/32: non-Rebel damage survives projectile and source removal");
    if (!empty) expect(damaged_frame >= 4 && damaged_frame <= 16,
        "WNO-32: first recipient is delivered within randomized 0.125..0.5 second window");
    if (chain && !empty) expect(!session.durability_state(4), "WNO-31: nearby neutral Hutt container dies from area damage");
    return rows;
}

void test_death_payloads() {
    const eawr::sim::InlineExecutor inline_executor;
    for (const bool empty : {false, true}) for (const bool chain : {false, true}) {
        const auto reference = death_payload_trace(empty, chain, inline_executor);
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(death_payload_trace(empty, chain, executor) == reference,
                "WNO-29/31/32: death, delay and chain state/event bytes match with " + std::to_string(workers) + " workers");
        }
    }
    std::vector<Fixed> choices;
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        Battle battle(false, false);
        battle.replay.setup.seed = seed;
        battle.replay.setup.units = {{1, 1, 1, at(0)}};
        battle.replay.commands = {{{0, 1, 0}, {1}, t::DamagePayload{q(1000)}}};
        battle.combat.profiles[0].weapons.clear();
        t::ShotProfile first; first.speed = q(6); first.blast = payload().blast;
        auto second = first; second.blast.damage = q(300);
        auto third = first; third.blast.damage = q(450);
        death_payloads(battle.combat.profiles[0], {first, second, third}, q(30));
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            auto session = battle.make();
            const auto step = session.step(executor);
            expect(step && session.projectiles().size() == 1, "WNO-29: multiple entries produce exactly one payload");
            if (!step || session.projectiles().empty()) continue;
            const auto& projectile = session.projectiles().front();
            expect(projectile.blast.damage == q(150) || projectile.blast.damage == q(300)
                || projectile.blast.damage == q(450), "WNO-29: choice comes from authored list");
            const std::vector<std::string> bytes{step.value().state_sha256, step.value().snapshot->sha256()};
            if (workers == 1) { reference = bytes; choices.push_back(projectile.blast.damage); }
            else expect(bytes == reference, "WNO-29: synchronized list selection has equal state/event bytes");
        }
    }
    std::sort(choices.begin(), choices.end());
    expect(!choices.empty() && choices.front() != choices.back(), "WNO-29: list choice uses the synchronized draw");
    QueryFixture fixture;
    fixture.world.seed = 1774; fixture.world.frame = 8;
    for (eawr::sim::EntityId id = 1; id <= 10; ++id) fixture.add(id, 2, at(50));
    fixture.build();
    auto projectile = payload();
    explicit_delay(projectile, Fixed::from_raw(Fixed::scale / 2));
    auto prepared = d::prepare_blast(fixture.world, projectile, at(0));
    expect(static_cast<bool>(prepared), "WNO-32: explicit-delay recipient query succeeds");
    if (prepared) {
        std::vector<Fixed> delays;
        for (const auto& recipient : recipients(prepared.value())) {
            expect(recipient.delay.raw() >= Fixed::scale / 8 && recipient.delay.raw() <= Fixed::scale / 2,
                "WAD-26: every delivery delay lies within one quarter to full authored value");
            delays.push_back(recipient.delay);
        }
        std::sort(delays.begin(), delays.end());
        expect(!delays.empty() && delays.front() != delays.back(),
            "WNO-32: equal-distance recipients draw separate delays");
    }
}

std::vector<std::string> delayed_metadata_trace(const eawr::sim::PartitionExecutor& executor) {
    Battle battle(false, false);
    battle.replay.setup.units = {{1, 1, 1, at(0)}, {2, 2, 2, at(100)}};
    auto& shot = *battle.combat.profiles[0].weapons[0].shot;
    shot.max_travel = q(25); shot.blast.max_delay = q(1);
    // DG-23 adds target soft radius to the cap; omit geometry so expiry stays at x=25.
    battle.combat.profiles[1].collision.reset();
    shot.shield_damage = false; shot.hitpoint_damage = false; shot.energy_damage = true;
    auto& recipient = battle.health.profiles[1];
    recipient.max_shields = q(50); recipient.powered = true; recipient.max_energy = q(50);
    recipient.armor_type = recipient.shield_armor_type = 0;
    battle.health.damage->damage_types = battle.health.damage->armor_types = 1;
    battle.health.damage->armor_mods = {Fixed::from_raw(Fixed::scale / 2)};
    battle.health.damage->diminishing = {{Fixed{}, q(2)}, {q(1), q(2)}};
    shot.damage_type = 0;
    auto session = battle.make();
    std::vector<std::string> rows;
    std::optional<std::uint64_t> detonation;
    bool delivered = false;
    for (std::uint64_t frame = 0; frame < 70; ++frame) {
        auto step = session.step(executor);
        expect(static_cast<bool>(step), "WAD-26: delayed metadata witness steps");
        if (!step) break;
        if (step.value().blast_detonations != 0) {
            detonation = frame;
            const auto& terminal = step.value().snapshot->combat_events();
            expect(std::any_of(terminal.begin(), terminal.end(), [](const auto& event) {
                return event.kind == t::CombatEventKind::projectile_expired && event.aim.x == q(25);
            }), "WAD-26: distance-delay witness detonates 75 units from its recipient");
            expect(static_cast<bool>(session.submit({{frame + 1, 1, 1}, {1}, t::DamagePayload{q(1000)}})),
                "WAD-26: remove source between queue and delivery");
        }
        const auto health = session.durability_state(2);
        if (detonation && frame < *detonation + 11)
            expect(health && health->hull == q(1000) && health->shields == q(50),
                "WAD-26: distance-delay record waits for truncated frame count");
        if (detonation && frame == *detonation + 11) {
            delivered = true;
            expect(health && health->hull == q(975) && health->shields == Fixed{}
                && health->energy == q(50) && !health->last_hit_frame,
                "WAD-26: retain armor type; discard source flags, energy drain and diminishing context");
        }
        rows.push_back(step.value().state_sha256 + step.value().snapshot->sha256());
    }
    expect(detonation && delivered && !session.durability_state(1),
        "WAD-26: source-less queued damage survives source removal and truncates 11.25 frames to 11");
    return rows;
}

void test_large_distance_delay() {
    std::vector<std::uint8_t> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        Battle battle(false, true);
        battle.replay.setup.units = {{1, 1, 1, at(0)}, {2, 2, 2, at(4096)}};
        auto& shooter = battle.combat.profiles[0];
        shooter.max_attack_distance = shooter.weapons[0].range = q(5000);
        auto& shot = *shooter.weapons[0].shot;
        shot.blast.radius = Fixed::from_raw(1);
        shot.blast.max_delay = q(1);
        shot.blast.dropoff = false;
        battle.combat.profiles[1].collision = t::CollisionBox{at(-4096, -1, -1), at(4096, 1, 1)};
        auto session = battle.make();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::uint64_t detonation_frame{};
        bool launched = false;
        for (; detonation_frame < 30 && session.projectiles().empty(); ++detonation_frame) {
            const auto launch = session.step(executor);
            expect(static_cast<bool>(launch), "WAD-17: large-box delay witness steps toward launch");
            if (!launch) break;
            launched = !session.projectiles().empty();
        }
        expect(launched && session.projectiles().size() == 1 && session.projectiles()[0].position == at(0),
            "WAD-17: large-box delay witness launches at the origin");
        if (!launched || session.projectiles().size() != 1) continue;
        expect(session.request_projectile_explosion(session.projectiles()[0].id),
            "WAD-17: large-box delay witness requests detonation");
        const auto step = session.step(executor);
        expect(step && step.value().blast_detonations == 1,
            "WAD-17: large-box delay witness detonates once");
        if (!step) continue;
        const auto bytes = session.canonical_state_bytes();
        constexpr std::array<std::uint8_t, 4> tag{'B', 'L', 'S', 'T'};
        const auto block = std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end());
        expect(block != bytes.end() && bytes.end() - block >= 68,
            "WAD-17: overlapping box admits one queued delivery outside the tiny radius");
        if (block == bytes.end() || bytes.end() - block < 68) continue;
        const auto read = [&](const std::size_t offset, const std::size_t width) {
            std::uint64_t value{};
            for (std::size_t byte = 0; byte < width; ++byte)
                value |= static_cast<std::uint64_t>(block[static_cast<std::ptrdiff_t>(offset + byte)]) << (byte * 8);
            return value;
        };
        expect(read(4, 4) == 3 && read(12, 8) == 1 && read(28, 8) == 2,
            "WAD-17: BLST v3 retains the large-box recipient");
        expect(read(44, 8) == (std::uint64_t{1} << 60),
            "WAD-17: admitted centre distance produces representable delay raw 2^60");
        expect(read(20, 8) == detonation_frame + 2061584302080ULL,
            "WAD-26: large delay truncates to 2061584302080 frames without overflow");
        const auto health = session.durability_state(2);
        expect(health && health->hull == q(1000), "WAD-17: distant delivery remains pending");
        if (workers == 1) reference = bytes;
        else expect(bytes == reference, "WAD-17: large-delay canonical queue matches on 1/2/4/8 workers");
    }
}

void test_delayed_metadata() {
    const eawr::sim::InlineExecutor inline_executor;
    const auto reference = delayed_metadata_trace(inline_executor);
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(delayed_metadata_trace(executor) == reference,
            "WAD-26: retained metadata, source loss and due ticks have equal 1/2/4/8 bytes");
    }
    Battle direct(false, false);
    direct.replay.final_tick_count = 100;
    explicit_delay(*direct.combat.profiles[0].weapons[0].shot, Fixed::from_raw(Fixed::scale / 2));
    const auto direct_reference = trace(direct, inline_executor, false, true);
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(trace(direct, executor, false, true) == direct_reference,
            "WAD-26: direct and secondary explicit delays survive source loss with equal worker bytes");
    }
}
} // namespace

int main() {
    test_primary_and_cancellation(); test_tiers(); test_relationships_cap_and_source();
    test_bounds_refinement_and_delay(); test_session_determinism();
    test_source_death_between_players(); test_hardpoint_selection_and_routing(); test_hardpoint_session_determinism();
    test_shield_generator_split();
    test_rocket_blast_determinism();
    test_death_payloads();
    test_delayed_metadata();
    test_large_distance_delay();
    if (failures) return 1;
    std::cout << "blast contracts passed (WAD, workers 1/2/4/8, detonation-only work)\n";
    return 0;
}
