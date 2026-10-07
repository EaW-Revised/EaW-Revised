#include "damage_support.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"
#include <tuple>

namespace damage_test_support {

void test_first_projectile_contact() {
    namespace d = tactical::detail;
    tactical::CombatProfile profile;
    profile.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
    std::vector<tactical::SnapshotPlayer> players{{1, 1, false}, {2, 2, false}, {3, 3, false}};
    d::CombatWorld world;
    world.relationships = players;
    for (const auto& [id, owner, x, y] : {
             std::tuple{1U, 2U, 20, 0}, std::tuple{2U, 2U, 70, 0},
             std::tuple{3U, 3U, 10, 0}, std::tuple{4U, 2U, 50, 30}}) {
        d::CombatUnit object;
        object.id = id;
        object.owner = owner;
        object.profile = &profile;
        object.position = at(x, y);
        object.transform = math::to_matrix(math::identity_quat(), object.position).value();
        world.units.push_back(object);
    }
    d::CollectionTrees tree;
    const auto update = [&](const std::uint64_t frame) {
        std::vector<d::CollectionTrees::Member> members;
        for (const auto& object : world.units) {
            const auto x = object.position.x.raw(), y = object.position.y.raw();
            members.push_back({object.id, object.owner, {{Fixed::from_raw(x - units(5).raw()),
                Fixed::from_raw(y - units(5).raw()), units(-5)},
                {Fixed::from_raw(x + units(5).raw()), Fixed::from_raw(y + units(5).raw()), units(5)}}});
        }
        tree.update(members, frame);
    };
    update(0);
    world.projectile_collection = &tree;
    tactical::Projectile projectile;
    projectile.owner = 1;
    projectile.step = at(100, 0);
    projectile.max_travel = units(200);
    d::ProjectileScratch scratch;
    auto contact = d::step_projectile(world, projectile, scratch);
    // Q24 rounds 65/100 before scaling by the 100-unit step: allow 1e-5 units here.
    // The selected entity and candidate/exact-test counters below remain exact.
    expect(contact && contact.value().hit == 2 && close_to(contact.value().contact.x, 65.0, 1e-5),
        "DG-30: player 2 precedes nearer player 3; its later-inserted farther object precedes object 1");
    expect(scratch.exact_count == 1 && scratch.candidate_count == 3,
        "DG-30: off-ray object is culled and exact tests stop at the first eligible contact");
    std::vector<std::uint8_t> before, after;
    tree.append_state(before);
    static_cast<void>(d::step_projectile(world, projectile, scratch));
    tree.append_state(after);
    expect(before == after, "DG-30: projectile queries preserve tree history byte-for-byte");
    world.units.erase(world.units.begin() + 1);
    update(1);
    contact = d::step_projectile(world, projectile, scratch);
    expect(contact && contact.value().hit == 1, "DG-30: a removed craft leaves its collection");
    world.units[0].position = at(20, 30);
    world.units[0].transform = math::to_matrix(math::identity_quat(), world.units[0].position).value();
    update(2);
    contact = d::step_projectile(world, projectile, scratch);
    expect(contact && contact.value().hit == 3, "DG-30: moved boxes leave the ray; next hostile owner is considered");
    profile.living_projectile_collision = false;
    scratch = {};
    contact = d::step_projectile(world, projectile, scratch);
    expect(contact && !contact.value().hit && scratch.exact_count == 0,
        "DG-30: live permission is rechecked on contact independently of tree membership");
}

void test_segment() {
    const tactical::CollisionBox box{at(-10, -5, -5), at(10, 5, 5)};
    const auto identity = math::to_matrix(math::identity_quat(), at(100, 0)).value();
    auto entry = tactical::segment_enters_box(box, identity, at(80, 0), at(105, 0));
    expect(entry && entry.value() && close_to(*entry.value(), 10.0 / 25.0, 1e-6), "a segment enters the box at its face");
    entry = tactical::segment_enters_box(box, identity, at(60, 0), at(85, 0));
    expect(entry && !entry.value(), "a segment that stops short misses");
    entry = tactical::segment_enters_box(box, identity, at(80, 8), at(105, 8));
    expect(entry && !entry.value(), "a segment beside the box misses");
    entry = tactical::segment_enters_box(box, identity, at(100, 0), at(125, 0));
    expect(entry && entry.value() && entry.value()->raw() == 0, "a segment starting inside enters at 0");
    // Turned a quarter: the box's long axis now lies along Y.
    const math::Quat quarter{Fixed{}, Fixed{}, decimal("0.70710678"), decimal("0.70710678")};
    const auto turned = math::to_matrix(math::normalize(quarter).value(), at(100, 0)).value();
    entry = tactical::segment_enters_box(box, turned, at(100, -30), at(100, -5));
    expect(entry && entry.value() && close_to(*entry.value(), 20.0 / 25.0, 1e-4), "the box turns with its unit");
}


// WAD-02 regression: a blast-only primary contact needs the independent XML budget.
void test_zero_direct_blast() {
    for (const auto direct : {0, 7}) {
        auto table = combat();
        auto& shot = *table.profiles[0].weapons[0].shot;
        shot.damage = units(direct);
        shot.blast.damage = units(20);
        shot.blast.radius = units(200);
        shot.blast.max_delay = Fixed{};
        shot.shield_damage = false;
        table.profiles[0].weapons[0].pulse_count = 1;
        auto health = durability();
        health.profiles[2].hardpoints.clear();
        auto made = tactical::TacticalSession::create(
            setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(300, 0), true)}),
            sensors(), health, {}, std::nullopt, table);
        expect(static_cast<bool>(made), "WAD-02: fallback session creates");
        if (!made) continue;
        auto value = std::move(made).value();
        const eawr::sim::InlineExecutor executor;
        for (unsigned tick = 0; tick < 60; ++tick) {
            auto stepped = value.step(executor);
            expect(static_cast<bool>(stepped), "WAD-02: fallback step succeeds");
            if (!stepped) break;
            const auto events = stepped.value().snapshot->combat_events();
            if (std::any_of(events.begin(), events.end(), [](const auto& event) {
                    return event.kind == tactical::CombatEventKind::projectile_hit;
                })) {
                const auto state = value.durability_state(2);
                expect(state && state->hull == units(300 - 3 * (direct == 0 ? 20 : direct)),
                    "WAD-02: zero substitutes area budget; positive direct override wins");
                break;
            }
            expect(tick != 59, "WAD-02: contact occurs");
        }
    }
}

// #536, DG-36: a square plate in the local YZ plane at x = `x`, half size `half`.
[[nodiscard]] std::vector<tactical::CollisionTriangle> plate(const std::int64_t x, const std::int64_t half) {
    return {{at(x, -half, -half), at(x, half, -half), at(x, half, half)},
        {at(x, -half, -half), at(x, half, half), at(x, -half, half)}};
}

// Brute force: every triangle of every enabled mesh on its own, for the tree's check.
[[nodiscard]] std::optional<tactical::MeshHit> brute(const std::vector<tactical::CollisionMesh>& meshes,
    const math::Mat3x4& transform, const math::Vec3& from, const math::Vec3& to) {
    std::optional<tactical::MeshHit> best;
    for (std::size_t index = 0; index < meshes.size(); ++index) {
        for (const auto& triangle : meshes[index].triangles) {
            const std::vector<tactical::CollisionMesh> single{
                tactical::collision_mesh({triangle}, tactical::no_hardpoint, tactical::no_hardpoint, false)};
            const auto hit = tactical::segment_hits_meshes(single, [](std::size_t) { return true; }, transform, from, to);
            if (hit && hit.value() && (!best || hit.value()->fraction < best->fraction)) {
                best = tactical::MeshHit{hit.value()->fraction, index};
            }
        }
    }
    return best;
}

// DG-36a: exercise the actual projectile admission gate, not just a mesh's tree.
void test_projectile_mesh_budget() {
    namespace d = tactical::detail;
    tactical::CombatProfile profile;
    profile.collision = tactical::CollisionBox{at(-200, -200, -5), at(200, 200, 5)};
    profile.mesh_bounds = tactical::CollisionBox{at(0, -1, -1), at(0, 125, 1)};
    for (std::int64_t index = 0; index < 32; ++index) {
        auto triangles = plate(0, 1);
        for (auto& triangle : triangles) {
            for (auto* vertex : {&triangle.a, &triangle.b, &triangle.c}) {
                vertex->y = Fixed::from_raw(vertex->y.raw() + index * 4 * one);
            }
        }
        profile.meshes.push_back(tactical::collision_mesh(
            std::move(triangles), tactical::no_hardpoint, tactical::no_hardpoint, false));
    }
    std::vector<tactical::SnapshotPlayer> players{{1, 1, false}, {2, 2, false}};
    d::CombatWorld world;
    world.relationships = players;
    d::CombatUnit target;
    target.id = 2;
    target.owner = 2;
    target.profile = &profile;
    target.position = at(100, 0);
    target.transform = math::to_matrix(math::identity_quat(), target.position).value();
    world.units.push_back(target);
    d::CollectionTrees tree;
    const std::vector<d::CollectionTrees::Member> members{
        {target.id, target.owner, {at(-100, -200, -5), at(300, 200, 5)}}};
    tree.update(members, 0);
    world.projectile_collection = &tree;
    std::vector<std::uint8_t> before, after;
    tree.append_state(before);
    tactical::MeshCollisionWork admitted_work, rejected_work;
    d::ProjectileScratch scratch, uncounted;
    std::size_t hits = 0;
    for (std::int64_t sample = 0; sample < 400; ++sample) {
        // All four lanes enter the collection; only the first two enter combined bounds.
        const auto lane = sample % 4;
        const auto y = lane == 0 ? (sample / 4 % 32) * 4
            : lane == 1 ? (sample / 4 % 31) * 4 + 2 : lane == 2 ? 160 : -40;
        tactical::Projectile projectile;
        projectile.owner = 1;
        projectile.position = at(80, y);
        projectile.step = at(40, 0);
        projectile.speed = units(40);
        projectile.max_travel = units(200);
        auto& work = lane < 2 ? admitted_work : rejected_work;
        const auto counted = d::step_projectile(world, projectile, scratch, &work);
        const auto plain = d::step_projectile(world, projectile, uncounted);
        expect(counted && plain, "DG-36a: production projectile queries succeed");
        if (!counted || !plain) return;
        const auto& result = counted.value();
        expect(lane == 0 ? result.hit == target.id && result.meshed && result.contact.x == units(100)
                        : !result.hit,
            "DG-36a: production contact and both kinds of miss keep their expected results");
        hits += result.hit.has_value();
        std::vector<std::uint8_t> counted_bytes, plain_bytes;
        d::append_projectile(counted_bytes, result.projectile);
        d::append_projectile(plain_bytes, plain.value().projectile);
        expect(counted_bytes == plain_bytes && result.hit == plain.value().hit
                && result.mesh_hardpoint == plain.value().mesh_hardpoint
                && result.meshed == plain.value().meshed && result.contact == plain.value().contact
                && result.from == plain.value().from && result.expired == plain.value().expired,
            "DG-36a: optional diagnostics preserve projectile state and contact results");
    }
    tree.append_state(after);
    expect(before == after && hits == 100 && scratch.candidate_count == 400 && scratch.exact_count == 400,
        "DG-36a: all 400 shots reach production admission without changing collection history");
    expect(admitted_work.meshes == 6400 && admitted_work.boxes <= 6400 && admitted_work.triangles <= 400,
        "DG-36a: admitted production queries stay within the shots times meshes work budget");
    expect(rejected_work.meshes == 0 && rejected_work.boxes == 0 && rejected_work.triangles == 0,
        "DG-36a: combined-bounds misses admit zero production mesh work");
    std::cout << "production mesh work: admitted meshes " << admitted_work.meshes << ", boxes "
              << admitted_work.boxes << ", triangles " << admitted_work.triangles
              << "; combined-bounds misses meshes " << rejected_work.meshes << ", boxes "
              << rejected_work.boxes << ", triangles " << rejected_work.triangles << '\n';
}

void test_meshes() {
    test_projectile_mesh_budget();
    const auto all = [](std::size_t) { return true; };
    const auto placed = math::to_matrix(math::identity_quat(), at(100, 0)).value();
    std::vector<tactical::CollisionMesh> meshes{
        tactical::collision_mesh(plate(0, 5), tactical::no_hardpoint, tactical::no_hardpoint, false)};
    auto hit = tactical::segment_hits_meshes(meshes, all, placed, at(80, 0), at(105, 0));
    expect(hit && hit.value() && hit.value()->fraction.raw() == one * 4 / 5 && hit.value()->mesh == 0,
        "DG-36: a step meets the plate at its fraction");
    hit = tactical::segment_hits_meshes(meshes, all, placed, at(80, 6), at(105, 6));
    expect(hit && !hit.value(), "DG-36: a step beside the plate misses");
    hit = tactical::segment_hits_meshes(meshes, all, placed, at(60, 0), at(85, 0));
    expect(hit && !hit.value(), "DG-36: a step that stops short misses");
    hit = tactical::segment_hits_meshes(meshes, all, placed, at(100, -20, 1), at(100, 20, 1));
    expect(hit && !hit.value(), "DG-36: a step in the plate's plane is parallel and misses");
    hit = tactical::segment_hits_meshes(meshes, all, placed, at(120, 0), at(95, 0));
    expect(hit && hit.value() && hit.value()->fraction.raw() == one * 4 / 5, "DG-36: both faces of a triangle count");
    // A hardpoint plate in front of the hull plate wins; switched off, the hull plate is met.
    meshes.push_back(tactical::collision_mesh(plate(-5, 2), 0, 0, false));
    hit = tactical::segment_hits_meshes(meshes, all, placed, at(80, 0), at(105, 0));
    expect(hit && hit.value() && hit.value()->mesh == 1 && hit.value()->fraction.raw() == one * 3 / 5,
        "DG-36: the first mesh along the step wins");
    hit = tactical::segment_hits_meshes(
        meshes, [](std::size_t index) { return index != 1; }, placed, at(80, 0), at(105, 0));
    expect(hit && hit.value() && hit.value()->mesh == 0, "DG-38: a switched-off mesh lets the step through");
    // The mesh turns with its unit: a quarter turn puts the plate across Y.
    const math::Quat quarter{Fixed{}, Fixed{}, decimal("0.70710678"), decimal("0.70710678")};
    const auto turned = math::to_matrix(math::normalize(quarter).value(), at(100, 0)).value();
    hit = tactical::segment_hits_meshes(
        meshes, [](std::size_t index) { return index == 0; }, turned, at(100, -20), at(100, 5));
    expect(hit && hit.value() && close_to(hit.value()->fraction, 20.0 / 25.0, 1e-4), "DG-36: the mesh turns with its unit");

    // The tree finds what every triangle on its own finds: a 20 x 20 grid of small tilted plates.
    std::vector<tactical::CollisionTriangle> field;
    for (std::int64_t row = 0; row < 20; ++row) {
        for (std::int64_t column = 0; column < 20; ++column) {
            const auto x = row * 7 - 70;
            const auto y = column * 7 - 70;
            field.push_back({at(x, y, -2), at(x + 3, y, 1), at(x, y + 3, 2)});
        }
    }
    const std::vector<tactical::CollisionMesh> tree{
        tactical::collision_mesh(field, tactical::no_hardpoint, tactical::no_hardpoint, false)};
    expect(tree[0].nodes.size() > 1 && tree[0].triangles.size() == field.size(), "DG-36: the field gets a tree");
    expect(tactical::collision_mesh(field, tactical::no_hardpoint, tactical::no_hardpoint, false) == tree[0],
        "DG-36: the tree is built the same every time");
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    const auto next = [&]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<std::int64_t>(state % 2001) - 1000;
    };
    std::size_t agreed = 0;
    std::size_t met = 0;
    tactical::MeshCollisionWork work;
    for (int sample = 0; sample < 400; ++sample) {
        const math::Vec3 from{Fixed::from_raw(next() * one / 10 + 100 * one), Fixed::from_raw(next() * one / 10),
            Fixed::from_raw(next() * one / 50 + 10 * one)};
        const math::Vec3 to{Fixed::from_raw(next() * one / 10 + 100 * one), Fixed::from_raw(next() * one / 10),
            Fixed::from_raw(next() * one / 50 - 10 * one)};
        const auto fast = tactical::segment_hits_meshes(tree, all, placed, from, to, &work);
        const auto slow = brute(tree, placed, from, to);
        agreed += fast && fast.value().has_value() == slow.has_value()
            && (!slow || fast.value()->fraction == slow->fraction);
        met += slow.has_value();
    }
    expect(agreed == 400 && met > 20, "DG-36: the tree finds what the brute force finds (" + std::to_string(met) + " met)");
    // DG-36a: fixed work budget, never elapsed time. A linear scan tests 160000 triangles.
    expect(work.meshes == 400 && work.boxes <= 40000 && work.triangles <= 20000,
        "DG-36a: 400 queries stay within the mesh collision work budget");
    std::cout << "mesh collision work: meshes " << work.meshes << ", boxes " << work.boxes
              << ", triangles " << work.triangles << '\n';
    tactical::MeshCollisionWork miss_work;
    const auto miss = tactical::segment_hits_meshes(tree, all, placed, at(0, 200), at(200, 200), &miss_work);
    expect(miss && !miss.value() && miss_work.meshes == 1 && miss_work.boxes == 1 && miss_work.triangles == 0,
        "DG-36a: a root-box miss does no triangle work");
    tactical::MeshCollisionWork disabled_work;
    const auto disabled = tactical::segment_hits_meshes(tree, [](std::size_t) { return false; }, placed,
        at(0, 0), at(200, 0), &disabled_work);
    expect(disabled && !disabled.value() && disabled_work.meshes == 0 && disabled_work.boxes == 0
            && disabled_work.triangles == 0, "DG-36a: disabled meshes do no collision work");

    // Validation: meshes need the box and their bounds, and must stay within 4096 units.
    auto table = combat();
    table.profiles[1].meshes = {tactical::collision_mesh(plate(0, 5), tactical::no_hardpoint, tactical::no_hardpoint, false)};
    expect(!tactical::validate_combat(table), "DG-36: meshes without their bounds are refused");
    table.profiles[1].mesh_bounds = tactical::CollisionBox{at(0, -5, -5), at(0, 5, 5)};
    expect(static_cast<bool>(tactical::validate_combat(table)), "DG-36: a mesh with its box and bounds is accepted");
    table.profiles[1].meshes = {tactical::collision_mesh(plate(5000, 5), tactical::no_hardpoint, tactical::no_hardpoint, false)};
    expect(!tactical::validate_combat(table), "DG-36: a mesh beyond 4096 units is refused");
}

// #536, DG-11, DG-36, DG-38 in a session: the corvette of DG-F2 faces the shooter with a hardpoint
// plate at its hardpoint (local x 15) in front of a hull plate (local x 10). Hits destroy the
// hardpoint first; its plate then stops colliding and the hull plate takes the rest. The frigate
// gets a shield plate in front (local x 18): the shield takes every hit while it is up, and the
// hardpoint and hull behind it are untouched.
void test_mesh_hits() {
    auto table = combat();
    for (const auto index : {std::size_t{1}, std::size_t{2}}) {
        auto& profile = table.profiles[index];
        profile.meshes = {tactical::collision_mesh(plate(10, 9), tactical::no_hardpoint, tactical::no_hardpoint, false),
            tactical::collision_mesh(plate(15, 4), 0, 0, false)};
        if (index == 1) {
            profile.meshes.push_back(tactical::collision_mesh(plate(18, 9), tactical::no_hardpoint, tactical::no_hardpoint, true));
        }
        profile.mesh_bounds = tactical::CollisionBox{at(10, -9, -9), at(18, 9, 9)};
    }
    const auto make = [&](const tactical::TypeId type) {
        auto created = tactical::TacticalSession::create(
            setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, type, 2, at(300, 0), true)}), sensors(), durability(), {},
            std::nullopt, table);
        expect(static_cast<bool>(created), "mesh session is created");
        return std::move(created).value();
    };
    auto corvette = make(corvette_type);
    auto health = corvette.durability_state(2);
    for (int tick = 0; tick < 200 && health && health->hardpoints[0].raw() > 0; ++tick) {
        static_cast<void>(run(corvette, 1));
        health = corvette.durability_state(2);
    }
    expect(health && health->hardpoints[0].raw() == 0 && health->hull.raw() == 300 * one,
        "DG-11: hits on the hardpoint's mesh destroy it and leave the hull");
    auto tally = run(corvette, 200);
    health = corvette.durability_state(2);
    expect(!health || health->hull.raw() < 300 * one, "DG-38: the destroyed hardpoint's mesh lets the shots reach the hull");
    auto shielded = make(frigate_type);
    tally = run(shielded, 60);
    health = shielded.durability_state(2);
    expect(health && health->shields.raw() < 100 * one && health->hardpoints[0].raw() == 90 * one
            && health->hull.raw() == 600 * one && tally.absorbed == tally.hits && tally.hits > 0,
        "DG-38: the shield mesh takes the hits while the shield is up");
    auto storm_health = durability();
    storm_health.damage->shield_recharge_frames = 1;
    storm_health.damage->ion_storm_disable_seconds = units(5);
    tactical::MotionTable motion;
    motion.rules = {units(15), units(300)};
    motion.avoidance = tactical::AvoidanceRules{units(24), decimal("0.2"), units(100), decimal("0.8"), units(15),
        decimal("0.66"), decimal("1.2"), decimal("0.25"), decimal("1.7"), decimal("0.5"), decimal("0.5"), 3500, 6, 90, 45, units(50)};
    tactical::Footprint storm;
    storm.type_id = 99;
    storm.layer = tactical::SpaceLayer::static_object;
    storm.radius = units(100);
    storm.obstacle = storm.ion_storm = true;
    motion.footprints = {storm};
    auto created = tactical::TacticalSession::create(setup({unit(1, shooter_type, 1, at(0, 0)),
        unit(2, frigate_type, 2, at(300, 0), true), unit(10, 99, 1, at(300, 0))}), sensors(), storm_health, motion, std::nullopt, table);
    expect(created.has_value(), "WHZ-33: storm mesh collision fixture validates");
    if (created) {
        // The existing weapon's initial recharge phase can exceed twenty frames.
        tally = run(created.value(), 60);
        health = created.value().durability_state(2);
        expect(tally.hits > 0 && tally.absorbed == 0 && health && health->shields == units(100)
            && health->hardpoints[0] < units(90),
            "WHZ-33: projectiles pass a disabled shield mesh and reach the hardpoint while the shield pool stays positive");
    }
}

// #607, DG-37: the craft sphere is tested only for a step that meets the craft's world box (its
// collision box turned by the craft, then axis-aligned). A station-shaped craft (box +-10, a small
// plate for its mesh, Collision_Box_Modifier 2) sits 12 units beside the shooter's path to a
// frigate. Unturned, its box stops 2 units short of the path, so the shots pass it although the
// path runs within its sphere (radius 20), and the frigate takes them. Turned 45 degrees about Z,
// its world box reaches 14.1 units from its centre and the path crosses it: the sphere (radius
// 28.3) catches the shots. The shooter may not fire at the station, so every shot aims past it.
[[nodiscard]] std::pair<bool, bool> sphere_run(const bool turned) {
    auto table = combat();
    table.profiles[0].weapons[0].category_restrictions = 4;
    auto& station = table.profiles[3];
    station.category_bits = 4;
    station.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
    station.meshes = {tactical::collision_mesh(plate(0, 1), tactical::no_hardpoint, tactical::no_hardpoint, false)};
    station.mesh_bounds = tactical::CollisionBox{at(0, -1, -1), at(0, 1, 1)};
    station.sphere_modifier = units(2);
    auto beside = unit(3, station_type, 2, at(200, 12));
    if (turned) {
        beside.rotation = math::normalize(math::Quat{Fixed{}, Fixed{}, decimal("0.38268343"), decimal("0.92387953")}).value();
    }
    auto created = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(400, 0), true), beside}), sensors(),
        durability(), {}, std::nullopt, table);
    expect(static_cast<bool>(created), "sphere session is created");
    if (!created) return {false, false};
    auto value = std::move(created).value();
    const auto tally = run(value, 90);
    expect(tally.shots > 0 && tally.hits > 0, "the sphere run fires and hits");
    const auto frigate_health = value.durability_state(2);
    const auto station_health = value.durability_state(3);
    return {frigate_health && frigate_health->shields.raw() < 100 * one,
        station_health && station_health->shields.raw() < 100 * one};
}

void test_craft_sphere() {
    const auto [frigate_hit, station_hit] = sphere_run(false);
    expect(frigate_hit && !station_hit, "DG-37: a path within the sphere but outside the world box passes the craft");
    const auto [frigate_hit_turned, station_hit_turned] = sphere_run(true);
    expect(station_hit_turned && !frigate_hit_turned, "DG-37: the turned craft's world box lets its sphere catch the path");
    auto table = combat();
    table.profiles[3].sphere_modifier = units(1);
    expect(!tactical::validate_combat(table), "DG-37: a modifier of 1 is refused");
    table.profiles[3].sphere_modifier = units(257);
    expect(!tactical::validate_combat(table), "DG-37: a modifier above the limit is refused");
    table.profiles[3].sphere_modifier = units(250);
    expect(static_cast<bool>(tactical::validate_combat(table)), "DG-37: the Death Star's 250 is accepted");
}

// #607, DG-24: the object weapon scatters too. Its muzzle sits at the shooter's centre and the
// corvette's hardpoint (its aim point) 285 units away; with an inaccuracy of 30 (doubled: 60) and a
// range of 700, every shot lands within 285 x 60 / 700 = 24.4 units of it on each axis, height
// included.
void test_object_weapon_scatter() {
    auto table = combat();
    auto gun = laser();
    gun.hardpoint = tactical::object_weapon;
    gun.cone_width = Fixed{};
    gun.cone_height = Fixed{};
    gun.fire_a = at(0, 0);
    gun.shot->inaccuracy = {{2, units(30)}};
    table.profiles[0].weapons = {gun};
    auto created = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(300, 0), true)}), sensors(), durability(),
        {}, std::nullopt, table);
    expect(static_cast<bool>(created), "object weapon scatter session is created");
    if (!created) return;
    auto value = std::move(created).value();
    const eawr::sim::InlineExecutor executor;
    const auto radius = 285.0 * 60.0 / 700.0;
    std::size_t shots = 0;
    bool within = true;
    double widest = 0.0;
    double highest = 0.0;
    for (int tick = 0; tick < 60; ++tick) {
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "object weapon scatter step succeeds");
        if (!stepped) return;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            if (event.kind != tactical::CombatEventKind::weapon_fired) continue;
            ++shots;
            const auto offset = [&](const Fixed value, const std::int64_t centre) {
                return std::abs(static_cast<double>(value.raw() - centre * one) / static_cast<double>(one));
            };
            const auto dx = offset(event.aim.x, 285);
            const auto dy = offset(event.aim.y, 0);
            const auto dz = offset(event.aim.z, 0);
            within = within && dx <= radius + 1e-3 && dy <= radius + 1e-3 && dz <= radius + 1e-3;
            widest = std::max({widest, dx, dy, dz});
            highest = std::max(highest, dz);
        }
    }
    expect(shots >= 3 && within, "DG-24: the object weapon's shots land within its scatter radius");
    expect(widest > radius / 4 && highest > 0.5, "DG-24: the object weapon's shots scatter on every axis");
}

// W-06a (#607): the object weapon's burst clock. A three-pulse object weapon (no cone, 6 frames
// between pulses, recharge 1 s) fires at a corvette that outlasts the run.
// - With nothing to stop it, every burst fires three shots 6 frames apart, and the next burst
//   follows the last shot after 30 frames plus a synchronized draw of 0 to 10: both bounds occur.
// - With a 40-energy shot, a pool of 100 and 80 energy every recharge interval (150 frames, EN-02),
//   each later burst fires two shots and spends its third pulse unfired, since a begun burst spends
//   a pulse before the energy check; the next burst's first shot waits, unspent, for the pool
//   (EN-05). The shots come in pairs 150 frames apart. The old clock (a pulse spent only by a shot)
//   fires the held third pulse at the refill instead, and no pairs form.
void test_object_burst_clock() {
    auto table = combat();
    auto gun = laser();
    gun.hardpoint = tactical::object_weapon;
    gun.cone_width = Fixed{};
    gun.cone_height = Fixed{};
    gun.pulse_count = 3;
    gun.min_recharge_hundredths = 100;
    gun.max_recharge_hundredths = 100;
    const auto fired = [&](const tactical::CombatTable& weapons, const tactical::DurabilityTable& health, const std::uint64_t ticks) {
        std::vector<std::uint64_t> out;
        auto created = tactical::TacticalSession::create(
            setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(300, 0), true)}), sensors(), health, {},
            std::nullopt, weapons);
        expect(static_cast<bool>(created), "burst clock session is created");
        if (!created) return out;
        auto value = std::move(created).value();
        const eawr::sim::InlineExecutor executor;
        for (std::uint64_t tick = 0; tick < ticks; ++tick) {
            auto stepped = value.step(executor);
            expect(static_cast<bool>(stepped), "burst clock step succeeds");
            if (!stepped) break;
            for (const auto& event : stepped.value().snapshot->combat_events()) {
                if (event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 1) out.push_back(event.tick);
            }
        }
        return out;
    };
    auto health = durability();
    health.profiles[2].max_hull = units(1000000);

    table.profiles[0].weapons = {gun};
    const auto free = fired(table, health, 3000);
    std::vector<std::vector<std::uint64_t>> bursts;
    for (std::size_t index = 0; index < free.size(); ++index) {
        if (index == 0 || free[index] - free[index - 1] != 6) bursts.emplace_back();
        bursts.back().push_back(free[index]);
    }
    bool threes = bursts.size() > 40;
    std::uint64_t shortest = ~std::uint64_t{};
    std::uint64_t longest = 0;
    for (std::size_t index = 0; index + 1 < bursts.size(); ++index) {
        threes = threes && bursts[index].size() == 3;
        const auto gap = bursts[index + 1].front() - bursts[index].back();
        shortest = std::min(shortest, gap);
        longest = std::max(longest, gap);
    }
    expect(threes, "W-06: every burst fires its three pulses 6 frames apart (" + std::to_string(bursts.size()) + " bursts)");
    expect(shortest == 30 && longest == 40, "W-06a: the recharge is 30 frames plus a draw of 0 to 10, both bounds seen ("
            + std::to_string(shortest) + " to " + std::to_string(longest) + ")");

    auto costly = gun;
    costly.shot->energy_per_shot = units(40);
    table.profiles[0].weapons = {costly};
    health.profiles[0].powered = true;
    health.profiles[0].max_energy = units(100);
    health.profiles[0].energy_refresh = units(80);
    const auto powered = fired(table, health, 1200);
    std::vector<std::vector<std::uint64_t>> groups;
    for (std::size_t index = 0; index < powered.size(); ++index) {
        if (index == 0 || powered[index] - powered[index - 1] != 6) groups.emplace_back();
        groups.back().push_back(powered[index]);
    }
    bool pairs = groups.size() > 6;
    for (std::size_t index = 1; index < groups.size(); ++index) {
        pairs = pairs && groups[index].size() == 2;
        if (index >= 2) pairs = pairs && groups[index].front() - groups[index - 1].front() == 150;
    }
    std::string shape;
    for (const auto& group : groups) shape += ' ' + std::to_string(group.front()) + 'x' + std::to_string(group.size());
    expect(pairs, "W-06a, EN-05: a begun burst spends its third pulse unfired and the next first shot waits for energy:" + shape);
}

// #669, DG-39: the corvette's hull plate (local x 18) now stands in front of its hardpoint plate
// (local x 10). Every shot aims at the hardpoint and meets the hull plate first. With the aimed
// route the hardpoint takes every hit and the hull none until the hardpoint is destroyed; the
// shots that follow aim at the hull and wear it down. Without the route (the #536 rule alone)
// the hull plate sends every hit to the hull and the hardpoint is never touched.
void test_aimed_routes() {
    // DG-36b/CO-11: the projectile union must not change the ordinary tree's
    // serialized bounds/history or equal-priority AI selection at its rebuild.
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        tactical::CombatTable table;
        std::vector<tactical::UnitState> staged;
        std::vector<tactical::SensorProfile> revealed;
        std::vector<tactical::detail::CollectionTrees::Member> parent_members;
        for (std::uint64_t id = 1; id <= 20; ++id) {
            tactical::CombatProfile candidate;
            candidate.type_id = 1000 + id;
            candidate.category_bits = 2;
            const auto x = static_cast<std::int64_t>(id) * 20;
            candidate.collision = tactical::CollisionBox{at(x - 101, -1, -1), at(x - 99, 1, 1)};
            if (id == 20) candidate.mesh_bounds = tactical::CollisionBox{at(x - 101, -1, -941), at(x - 99, 1, 1)};
            table.profiles.push_back(candidate);
            staged.push_back(unit(id, candidate.type_id, 2, at(100, 0)));
            revealed.push_back({candidate.type_id, units(2000)});
            parent_members.push_back({id, 2, {at(x - 1, -1, -1), at(x + 1, 1, 1)}});
        }
        tactical::CombatProfile scanner;
        scanner.type_id = 2000;
        scanner.max_attack_distance = units(2000);
        scanner.collision = tactical::CollisionBox{at(-1, -1, -1), at(1, 1, 1)};
        scanner.weapons = {laser()};
        table.profiles.push_back(scanner);
        staged.push_back(unit(100, scanner.type_id, 1, at(0, 0)));
        revealed.push_back({scanner.type_id, units(2000)});
        parent_members.push_back({100, 1, {at(-1, -1, -1), at(1, 1, 1)}});
        auto created = tactical::TacticalSession::create(setup(staged), revealed, {}, {}, std::nullopt, table);
        expect(static_cast<bool>(created), "DG-36b: ordinary-tree invariant session starts");
        if (!created) continue;
        auto world = std::move(created).value();
        tactical::detail::CollectionTrees parents;
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (std::uint64_t frame = 0; frame <= 31; ++frame) {
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "DG-36b: ordinary-tree invariant step succeeds");
            if (!step) break;
            parents.update(parent_members, frame);
            std::vector<std::uint8_t> expected{'C', 'U', 'L', 'L'};
            parents.append_state(expected);
            const auto bytes = world.canonical_state_bytes();
            const std::array<std::uint8_t, 4> tag{'C', 'U', 'L', 'L'};
            const auto found = std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end());
            expect(found != bytes.end() && static_cast<std::size_t>(bytes.end() - found) >= expected.size()
                && std::equal(expected.begin(), expected.end(), found),
                "DG-36b: ordinary collection bytes keep parent bounds and persistent history");
            const auto state = world.combat_state(100);
            expect(state && state->attack_target == 20,
                "CO-11: attachment bounds never displace the first tied AI target after the rebuild");
        }
    }

    // DG-36b: both shot paths approach attached geometry outside the parent hull box.
    for (const bool homing : {false, true}) {
        const auto attached = [&](const bool miss, const bool destroyed) {
            auto table = combat();
            auto& weapon = table.profiles[0].weapons[0];
            weapon.fire_a = at(0, 0, miss ? -80 : -60);
            weapon.shot->homing = homing;
            weapon.shot->turn_rate = homing ? units(3) : Fixed{};
            auto& profile = table.profiles[2];
            profile.hardpoints = {{0, at(0, 0, miss ? -80 : -60), true}};
            auto triangles = plate(-5, 4);
            for (auto& triangle : triangles) {
                for (auto* corner : {&triangle.a, &triangle.b, &triangle.c}) {
                    corner->z = math::add(corner->z, units(-60)).value();
                }
            }
            profile.meshes = {tactical::collision_mesh(std::move(triangles), 0, 0, false)};
            profile.mesh_bounds = tactical::CollisionBox{at(-5, -4, -64), at(-5, 4, -56)};
            profile.aimed_routes = {0};
            auto health = durability();
            health.profiles[2].destroyed_with_hardpoints = false;
            auto created = tactical::TacticalSession::create(
                setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(300, 0))}),
                sensors(), health, {}, std::nullopt, table);
            expect(static_cast<bool>(created), "DG-36b: detached-bound hardpoint session binds");
            auto value = std::move(created).value();
            if (destroyed) {
                expect(static_cast<bool>(value.submit({{0, 2, 0}, {2},
                    tactical::DamagePayload{units(90), 0}})), "DG-36b: generator destruction submits");
            }
            return value;
        };
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto value = attached(false, false);
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            std::size_t hits = 0;
            for (unsigned frame = 0; frame < 180; ++frame) {
                auto stepped = value.step(executor);
                expect(static_cast<bool>(stepped), "DG-36b: attached-bound combat advances");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256 + stepped.value().snapshot->sha256());
                for (const auto& event : stepped.value().snapshot->combat_events()) {
                    hits += event.kind == tactical::CombatEventKind::projectile_hit;
                }
            }
            const auto health = value.durability_state(2);
            expect(hits > 0 && health && health->hardpoints[0] < units(90),
                "DG-36b: laser and homing missile hit a hardpoint outside the parent collision bounds");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "DG-36b: attached-bound hits agree on 1/2/4/8 workers");
        }
        for (const bool destroyed : {false, true}) {
            auto value = attached(!destroyed, destroyed);
            const auto tally = run(value, 180);
            const auto health = value.durability_state(2);
            expect(tally.hits == 0 && health && health->hull == units(300),
                "DG-36b: expanded bounds fabricate neither a mesh hit nor a destroyed-generator hit");
        }
    }
    const auto make = [](const bool routed) {
        auto table = combat();
        auto& profile = table.profiles[2];
        profile.meshes = {tactical::collision_mesh(plate(18, 9), tactical::no_hardpoint, tactical::no_hardpoint, false),
            tactical::collision_mesh(plate(10, 4), 0, 0, false)};
        profile.mesh_bounds = tactical::CollisionBox{at(10, -9, -9), at(18, 9, 9)};
        if (routed) profile.aimed_routes = {0};
        auto created = tactical::TacticalSession::create(
            setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(300, 0), true)}), sensors(),
            durability(), {}, std::nullopt, table);
        expect(static_cast<bool>(created), "aimed-route session is created");
        return std::move(created).value();
    };
    auto routed = make(true);
    auto health = routed.durability_state(2);
    bool hull_untouched = true;
    for (int tick = 0; tick < 200 && health && health->hardpoints[0].raw() > 0; ++tick) {
        static_cast<void>(run(routed, 1));
        health = routed.durability_state(2);
        hull_untouched = hull_untouched && health && health->hull.raw() == 300 * one;
    }
    expect(health && health->hardpoints[0].raw() == 0 && hull_untouched,
        "DG-39: shots aimed at the hardpoint damage it through the hull mesh and leave the hull");
    static_cast<void>(run(routed, 300));
    health = routed.durability_state(2);
    expect(!health || health->hull.raw() < 300 * one, "DG-39: once it is destroyed the shots aim at the hull");

    // Without the route the first hits land on the hull; the hardpoint behind the plate is untouched.
    auto unrouted = make(false);
    health = unrouted.durability_state(2);
    for (int tick = 0; tick < 200 && health && health->hull.raw() == 300 * one; ++tick) {
        static_cast<void>(run(unrouted, 1));
        health = unrouted.durability_state(2);
    }
    expect(health && health->hardpoints[0].raw() == 90 * one && health->hull.raw() < 300 * one,
        "DG-11 without a route: the met hull mesh takes the hit (hull "
            + std::to_string(health ? health->hull.raw() / one : -1) + ", hardpoint "
            + std::to_string(health ? health->hardpoints[0].raw() / one : -1) + ")");

    // Validation: a route names a hardpoint within the type's limit.
    auto table = combat();
    table.profiles[2].aimed_routes = {static_cast<std::uint32_t>(tactical::max_hardpoints_per_type)};
    expect(!tactical::validate_combat(table), "DG-39: a route beyond the hardpoint limit is refused");
}

// #531 (space-orders OR-20, OR-25) with #669 (DG-39): a player's attack on one hardpoint takes the
// aimed route. The corvette gets a second hardpoint (1, local x 5, behind hardpoint 0 and off its
// axis), each routed to itself; the hull plate (local x 18) stands in front of both. Unordered, the
// shooter aims at the nearest hardpoint, 0. Ordered onto hardpoint 1, every shot meets the hull
// plate and still damages hardpoint 1: hardpoint 0 and the hull are untouched until it is destroyed.
void test_ordered_hardpoint_route() {
    const auto make = [] {
        auto table = combat();
        auto& profile = table.profiles[2];
        profile.hardpoints = {{0, at(15, 0), true}, {1, at(5, 6), true}};
        profile.meshes = {tactical::collision_mesh(plate(18, 9), tactical::no_hardpoint, tactical::no_hardpoint, false),
            tactical::collision_mesh(plate(15, 4), 0, 0, false)};
        profile.mesh_bounds = tactical::CollisionBox{at(5, -9, -9), at(18, 9, 9)};
        profile.aimed_routes = {0, 1};
        auto health = durability();
        health.profiles[2].hardpoints.push_back(health.profiles[2].hardpoints.front());
        auto created = tactical::TacticalSession::create(
            setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(300, 0), true)}), sensors(), health,
            {}, std::nullopt, table);
        expect(static_cast<bool>(created), "ordered-hardpoint session is created");
        return std::move(created).value();
    };
    const auto intact = [](const std::optional<tactical::DurabilityState>& health, const std::size_t index) {
        return health && health->hardpoints[index].raw() == 90 * one;
    };

    auto unordered = make();
    static_cast<void>(run(unordered, 120));
    auto health = unordered.durability_state(2);
    expect(health && health->hardpoints[0].raw() < 90 * one && intact(health, 1),
        "OR-24: unordered, the shots go to the nearest hardpoint, 0");

    auto ordered = make();
    expect(static_cast<bool>(ordered.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2, 1}})),
        "OR-20: the attack on hardpoint 1 submits");
    health = ordered.durability_state(2);
    bool others_untouched = true;
    for (int tick = 0; tick < 400 && health && health->hardpoints[1].raw() > 0; ++tick) {
        static_cast<void>(run(ordered, 1));
        health = ordered.durability_state(2);
        others_untouched = others_untouched && intact(health, 0) && health->hull.raw() == 300 * one;
    }
    expect(health && health->hardpoints[1].raw() == 0 && others_untouched,
        "OR-25, DG-39: shots on the ordered hardpoint damage it through the hull mesh, not the hull or hardpoint 0");
}

// Fixture DG-F3, out-of-range miss: the laser's projectile travels only 200 units (its range here),
// the target is 300 away: every projectile expires short of it and nothing is hit.
void test_out_of_range_miss() {
    auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(300, 0), true)}), 200);
    Tally tally;
    std::vector<eawr::sim::EntityId> expired;
    const eawr::sim::InlineExecutor executor;
    for (unsigned tick = 0; tick < 90; ++tick) {
        const auto before = value.projectiles();
        const std::vector<tactical::Projectile> flying(before.begin(), before.end());
        const auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "WAD-07: expiry delivery step succeeds");
        if (!stepped) return;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            tally.shots += event.kind == tactical::CombatEventKind::weapon_fired;
            tally.hits += event.kind == tactical::CombatEventKind::projectile_hit;
            if (event.kind != tactical::CombatEventKind::projectile_expired) continue;
            expect(std::find(expired.begin(), expired.end(), event.target) == expired.end(),
                "WAD-07: a projectile emits exactly one terminal event");
            expired.push_back(event.target);
            const auto previous = std::find_if(flying.begin(), flying.end(), [&](const auto& projectile) {
                return projectile.id == event.target;
            });
            expect(previous != flying.end() && event.origin == previous->position
                && event.aim != event.origin && event.shooter == 1 && event.selected_target == 2
                && event.outcome == static_cast<std::uint32_t>(tactical::ProjectileExpiryReason::travel_limit),
                "WAD-07: terminal delivery retains identity, final segment and travel reason");
            expect(std::none_of(value.projectiles().begin(), value.projectiles().end(), [&](const auto& projectile) {
                return projectile.id == event.target;
            }), "WAD-07: terminal delivery survives the projectile's removal");
        }
    }
    const auto health = value.durability_state(2);
    expect(tally.shots > 0 && tally.hits == 0, "projectiles that run out of travel hit nothing");
    expect(health && health->shields.raw() == 100 * one && health->hull.raw() == 600 * one, "the target is unharmed");
    expect(value.projectiles().size() <= tally.shots, "expired projectiles leave the session");
    expect(!expired.empty() && expired.size() + value.projectiles().size() == tally.shots,
        "WAD-07: every removed miss has one presentation event and flying shots have none");
}

// DG-33 at the range boundary, as FoC does it: each frame tests the whole step for a hit before
// the travel is counted, so the step that ends the flight can still hit up to one step past the
// range. The laser fires from the shooter's centre at a hull-less station's centre along X; its
// travel is 210, not a multiple of the speed 25, so the last step spans 200 to 225.
[[nodiscard]] Tally boundary(const std::int64_t station_x) {
    auto table = combat(210);
    table.profiles[0].weapons[0].fire_a = at(0, 0);
    table.profiles[3].collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
    auto created = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, station_type, 2, at(station_x, 0), true)}), sensors(),
        durability(), {}, std::nullopt, table);
    expect(static_cast<bool>(created), "boundary session is created");
    if (!created) return {};
    auto value = std::move(created).value();
    auto tally = run(value, 60);
    for (const auto& projectile : value.projectiles()) {
        expect(projectile.travelled < projectile.max_travel, "a projectile past its travel is gone");
    }
    return tally;
}

void test_range_boundary() {
    const auto inside = boundary(205); // face at 195
    expect(inside.shots > 0 && inside.hits > 0, "a target just inside the range is hit");
    const auto last_step = boundary(225); // face at 215: past the range, inside the last step
    expect(last_step.shots > 0 && last_step.hits > 0, "the last step hits up to one step past the range (FoC)");
    const auto beyond = boundary(245); // face at 235: past the last step
    expect(beyond.shots > 0 && beyond.hits == 0, "a target beyond the last step is never hit");
}

// A projectile flies straight: another hostile unit in its path takes the hit (DG-32), a friendly
// one does not.
void test_path() {
    auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(400, 0), true),
        unit(3, station_type, 2, at(200, 0), true), unit(4, shooter_type, 1, at(100, 30))}));
    // Only the frigate is a target: make the station unattractive by keeping it out of the scan
    // order is not needed; any hit on the station proves the path rule.
    const auto tally = run(value, 60);
    const auto frigate_health = value.durability_state(2);
    const auto station_health = value.durability_state(3);
    const auto friendly = value.durability_state(4);
    expect(tally.hits > 0 && station_health && station_health->shields.raw() < 100 * one,
        "the unit in the path takes the projectiles");
    expect(frigate_health && frigate_health->shields.raw() == 100 * one, "the unit behind it is shielded by it");
    expect(friendly && friendly->hull.raw() == 2000 * one, "a friendly unit beside the path is never hit");
}

// #636, the projectile broad phase's budget: a station with a 1200-unit box far off the lane makes
// the largest collision reach (the index query's growth) 1800 units, so every shot at the frigate
// takes the station and three corvettes beyond the weapon's range from the index as well. Their own
// reach never meets the shot's segment, so only the frigate reaches the exact tests, and only
// the frigate is hit. The third corvette sits right above the lane, 1600 units up (#718's layer
// heights): the reach test is in 3-D, so a unit planar-on-the-lane is still rejected.
void test_broad_phase_reach() {
    auto table = combat();
    table.profiles[3].collision = tactical::CollisionBox{at(-600, -600, -600), at(600, 600, 600)};
    auto created = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(300, 0), true),
            unit(3, station_type, 2, at(150, 1500), true), unit(4, corvette_type, 2, at(100, 800), true),
            unit(5, corvette_type, 2, at(250, -850), true), unit(6, corvette_type, 2, at(150, 0, 1600), true)}),
        sensors(), durability(), {}, std::nullopt, table);
    expect(static_cast<bool>(created), "broad phase session is created");
    if (!created) return;
    auto value = std::move(created).value();
    const eawr::sim::InlineExecutor executor;
    std::uint64_t steps = 0;
    std::uint64_t candidates = 0;
    std::uint64_t exact = 0;
    std::size_t hits = 0;
    for (int tick = 0; tick < 90; ++tick) {
        const auto flying = value.projectiles().size();
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "broad phase step succeeds");
        if (!stepped) return;
        expect(stepped.value().projectile_exact_tests <= flying,
            "tick " + std::to_string(tick) + ": a shot tests at most the frigate exactly");
        steps += flying;
        candidates += stepped.value().projectile_candidates;
        exact += stepped.value().projectile_exact_tests;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            hits += event.kind == tactical::CombatEventKind::projectile_hit && event.target == 2;
        }
    }
    // DG-30: the actual world-box ray admits only the frigate, with no largest-type expansion.
    expect(steps > 0 && candidates <= steps && exact <= candidates && exact < steps,
        "the owned ray tree admits at most one candidate per shot and rejects all far units");
    expect(hits > 0, "the frigate is hit");
    for (const eawr::sim::EntityId id : {3, 4, 5, 6}) {
        const auto health = value.durability_state(id);
        const auto full = id == 3 ? 100 * one : 300 * one;
        expect(health && (id == 3 ? health->shields.raw() : health->hull.raw()) == full, "a far unit is never hit");
    }
}

// #636 against DG-37's worst case: a craft's sphere hits a step that meets its world box, and a
// turned box's world box reaches sqrt(3) times its corner's length from the craft. A station-shaped
// craft with a +-100 box (corner length 173.2) and the Death Star's modifier of 250, turned 45
// degrees about X, has a world box of +-141.4 in Y and Z. It sits 140 units right and 140 up of
// the shooter's path to a frigate: the path crosses the world box 198 units from its centre, beyond
// the corner's length plus half a 25-unit step, and misses the box itself (198 units out along its
// turned diagonal). So only a reach that covers the world box keeps the craft in the exact tests;
// its sphere (radius 35,355) then takes every shot, and the frigate none.
void test_broad_phase_sphere_reach() {
    auto table = combat();
    table.profiles[0].weapons[0].category_restrictions = 4;
    auto& station = table.profiles[3];
    station.category_bits = 4;
    station.collision = tactical::CollisionBox{at(-100, -100, -100), at(100, 100, 100)};
    station.sphere_modifier = units(250);
    auto beside = unit(3, station_type, 2, at(200, 140, 140));
    beside.rotation = math::normalize(math::Quat{decimal("0.38268343"), Fixed{}, Fixed{}, decimal("0.92387953")}).value();
    auto created = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(400, 0), true), beside}), sensors(),
        durability(), {}, std::nullopt, table);
    expect(static_cast<bool>(created), "sphere reach session is created");
    if (!created) return;
    auto value = std::move(created).value();
    const auto tally = run(value, 90);
    const auto frigate_health = value.durability_state(2);
    const auto station_health = value.durability_state(3);
    expect(tally.shots > 0 && tally.hits > 0, "the sphere reach run fires and hits");
    expect(station_health && station_health->shields.raw() < 100 * one,
        "#636: the broad phase keeps a turned craft whose world box the path crosses beyond its corner");
    expect(frigate_health && frigate_health->shields.raw() == 100 * one, "the craft's sphere takes every shot");
}


} // namespace damage_test_support
