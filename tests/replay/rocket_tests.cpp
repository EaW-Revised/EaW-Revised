#include "../../src/sim/tactical/combat_internal.hpp"
#include "../../src/sim/tactical/collection.hpp"
#include "../../src/sim/tactical/rocket_internal.hpp"
#include "../../src/sim/tactical/motion_internal.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
namespace t = eawr::sim::tactical;
namespace d = t::detail;
namespace m = eawr::sim::math;
using m::Fixed;
int failures{};
Fixed q(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }
m::Vec3 at(const std::int64_t x, const std::int64_t y = 0, const std::int64_t z = 0) { return {q(x), q(y), q(z)}; }
m::Vec3 plus(const m::Vec3& a, const m::Vec3& b) {
    return {m::add(a.x, b.x).value(), m::add(a.y, b.y).value(), m::add(a.z, b.z).value()};
}
m::Vec3 minus(const m::Vec3& a, const m::Vec3& b) {
    return {m::subtract(a.x, b.x).value(), m::subtract(a.y, b.y).value(), m::subtract(a.z, b.z).value()};
}
void expect(const bool value, const std::string_view message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
t::FlightProfile rocket_profile() {
    t::FlightProfile flight;
    flight.kind = t::FlightKind::rocket; flight.target_radius = true;
    flight.authored_distance = q(5000); flight.curve_distance = q(500); flight.straight_distance = q(500);
    return flight;
}
t::Projectile launch(const t::FlightProfile& flight, const m::Vec3 aim = at(300), const Fixed travel = q(5000)) {
    t::CombatEvent event; event.shooter = 1; event.target = 2; event.aim = aim;
    t::ShotProfile shot; shot.speed = q(25); shot.max_travel = travel; shot.flight = flight;
    auto result = d::launch_projectile(event, shot, 1, true, 1, {});
    expect(static_cast<bool>(result), "RFL: launch accepts supported profile");
    return result ? result.value() : t::Projectile{};
}
void endpoint_and_retained_aim() {
    auto projectile = launch(rocket_profile());
    expect(!projectile.homing && projectile.flight && projectile.flight->aim == at(300),
        "WAD-04/RFL-01: rocket retains aim independently of a homing target");
    d::CollectionTrees collidables;
    d::CombatWorld world; d::ProjectileScratch scratch;
    world.projectile_collection = &collidables;
    bool terminal = false;
    for (std::uint32_t frame = 0; frame < 20; ++frame) {
        const auto before = projectile.position;
        auto step = d::step_projectile(world, projectile, scratch);
        expect(static_cast<bool>(step), "RFL-06: empty-world rocket step succeeds");
        if (!step) return;
        projectile = step.value().projectile;
        if (step.value().expired) {
            terminal = true;
            expect(projectile.position == before && projectile.position.x < q(300)
                && projectile.travelled < projectile.max_travel && !step.value().hit,
                "WAD-04/RFL-06: exhausted route expires at current pose before weapon travel limit");
            expect(step.value().expiry_reason == t::ProjectileExpiryReason::rocket_path_exhausted,
                "WAD-07: exhausted rocket lookup reports its terminal reason");
            break;
        }
    }
    expect(terminal, "RFL-06: retained route reaches a terminal state");
    expect(scratch.exact_count == 0, "RFL-06: no fabricated collision at exhausted endpoint");
    auto profile = rocket_profile(); profile.authored_distance = q(50);
    projectile = launch(profile);
    for (std::uint32_t frame = 0; frame < 2; ++frame) {
        auto step = d::step_projectile(world, projectile, scratch);
        expect(step && step.value().expired == (frame == 1), "RFL-08: authored distance terminates independently of longer path");
        if (step && step.value().expired) expect(step.value().expiry_reason == t::ProjectileExpiryReason::target_radius,
            "WAD-07: authored path distance reports target-radius expiry");
        if (!step) return;
        projectile = step.value().projectile;
    }
    expect(projectile.position.x > q(49) && projectile.position.x < q(51), "RFL-08: normal endpoint expiry uses updated pose");
}
void cubic_height_and_boundary() {
    auto projectile = launch(rocket_profile(), at(300, 0, 400));
    expect(static_cast<bool>(d::prepare_rocket_path(projectile)), "RFL-02: spatial route constructs");
    if (!projectile.flight) return;
    const auto& flight = *projectile.flight;
    expect(flight.path.size() == 2, "RFL-02/03: initial segment uses planar half-distance, subdivisions use spatial length");
    if (flight.path.empty()) return;
    Fixed total{};
    for (const auto& segment : flight.path) total = m::add(total, segment.length).value();
    expect(std::llabs(total.raw() - q(500).raw()) < 1000, "RFL-04: collinear spatial arc retains height");
    auto exact = d::rocket_point(flight, total);
    auto below = d::rocket_point(flight, Fixed::from_raw(total.raw() - 1));
    expect(exact && !exact.value() && below && below.value(), "RFL-04: strict whole-path endpoint lookup");
    const auto midpoint = d::rocket_point(flight, q(250));
    expect(midpoint && midpoint.value() && midpoint.value()->z.raw() > 0, "RFL-01/04: lookup supplies spatial height");
    auto tiny = launch(rocket_profile(), at(0, 0, 1));
    d::CollectionTrees collidables;
    d::CombatWorld world; d::ProjectileScratch scratch;
    world.projectile_collection = &collidables;
    auto terminal = d::step_projectile(world, tiny, scratch);
    expect(terminal && terminal.value().expired && terminal.value().projectile.position == at(0),
        "RFL-06: route with fewer than three control points terminates at current pose");
}
void default_endpoints_and_lifetime() {
    t::FlightProfile flight; flight.kind = t::FlightKind::default_projectile;
    flight.target_radius = true;
    auto projectile = launch(flight, at(50));
    d::CollectionTrees collidables;
    d::CombatWorld world; d::ProjectileScratch scratch;
    world.projectile_collection = &collidables;
    for (std::uint32_t frame = 0; frame < 3; ++frame) {
        auto step = d::step_projectile(world, projectile, scratch);
        expect(step && step.value().expired == (frame == 2), "RFL-08: DEFAULT expiry is strictly past spatial fire-at distance");
        if (!step) return;
        projectile = step.value().projectile;
    }
    flight.target_radius = false; flight.lifetime = Fixed::from_raw(Fixed::scale / 30);
    projectile = launch(flight, at(300), Fixed{});
    auto first = d::step_projectile(world, projectile, scratch);
    // Use an exact two-frame duration to avoid rounded seconds at the first-frame boundary.
    flight.lifetime = Fixed::from_raw(Fixed::scale * 2 / 30 + 1);
    projectile = launch(flight, at(300), Fixed{});
    for (std::uint32_t frame = 0; frame < 3; ++frame) {
        auto step = d::step_projectile(world, projectile, scratch);
        expect(step && step.value().expired == (frame == 2), "RFL-08: lifetime expires strictly after authored duration");
        if (step && step.value().expired) expect(step.value().expiry_reason == t::ProjectileExpiryReason::lifetime,
            "WAD-07: zero-distance allowance reports lifetime expiry");
        if (!step) return;
        projectile = step.value().projectile;
    }
    projectile = launch(flight, at(300), q(100));
    for (std::uint32_t frame = 0; frame < 4; ++frame) {
        auto step = d::step_projectile(world, projectile, scratch);
        expect(step && step.value().expired == (frame == 3), "RFL-08: positive travel allowance takes precedence over lifetime");
        if (step && step.value().expired) expect(step.value().expiry_reason == t::ProjectileExpiryReason::travel_limit,
            "WAD-07: positive allowance reports travel-limit expiry");
        if (!step) return;
        projectile = step.value().projectile;
    }
    expect(static_cast<bool>(first), "RFL-08: fractional lifetime service succeeds");
}
void impact_suppresses_expiry() {
    d::CollectionTrees collidables;
    const std::vector<d::CollectionTrees::Member> members{{2, 2, {at(40, -5, -5), at(55, 5, 5)}}};
    collidables.update(members, 0);
    t::CombatProfile profile;
    profile.collision = t::CollisionBox{at(40, -5, -5), at(55, 5, 5)};
    d::CombatUnit target;
    target.id = 2; target.owner = 2; target.profile = &profile;
    target.transform = m::to_matrix(m::identity_quat(), {}).value();
    d::CombatWorld world;
    const std::vector<t::SnapshotPlayer> players{{1, 1, false}, {2, 2, false}};
    world.relationships = players; world.units = {target}; world.projectile_collection = &collidables;
    d::ProjectileScratch scratch;
    auto flight = rocket_profile(); flight.authored_distance = q(50);
    auto projectile = launch(flight, at(300), q(50));
    projectile.owner = 1;
    auto first = d::step_projectile(world, projectile, scratch);
    expect(first && !first.value().expired && !first.value().hit, "WAD-04: first rocket step is clear");
    if (!first) return;
    const auto impact = d::step_projectile(world, first.value().projectile, scratch);
    expect(impact && impact.value().hit == 2 && !impact.value().expired
        && impact.value().expiry_reason == t::ProjectileExpiryReason::none,
        "WAD-04/07: an impact on the terminal step suppresses every expiry reason");
    projectile.explosion_requested = true;
    const auto explicit_end = d::step_projectile(world, projectile, scratch);
    expect(explicit_end && explicit_end.value().expired && !explicit_end.value().hit
        && explicit_end.value().projectile.position == projectile.position
        && explicit_end.value().expiry_reason == t::ProjectileExpiryReason::explicit_request,
        "WAD-05/07: an explicit request retains its current terminal pose");
}
void unsupported_and_encoding() {
    auto profile = rocket_profile();
    profile.curve_offset = q(1); expect(d::valid_flight(profile), "RFL-03: nonzero spatial offset is accepted");
    profile.curve_offset = {}; profile.target_radius = false;
    expect(d::valid_flight(profile), "RFL-05: post-aim extension is accepted");
    auto projectile = launch(rocket_profile());
    std::vector<std::uint8_t> before, after;
    d::append_projectile(before, projectile);
    expect(static_cast<bool>(d::prepare_rocket_path(projectile)), "RFL-04: path prepares for hash test");
    d::append_projectile(after, projectile);
    expect(before != after, "RFL-01/04: initialized path is hashed projectile state");
    before = after; after.clear(); projectile.flight->aim.z = q(1); d::append_projectile(after, projectile);
    expect(before != after, "RFL-01: retained aim height participates in projectile hash");
}

m::Vec3 spline_end(const t::FlightState& flight) {
    if (flight.path.empty()) return {};
    const auto& last = flight.path.back();
    return plus(last.a, plus(last.b, plus(last.c, last.d)));
}
Fixed path_length(const t::FlightState& flight) {
    Fixed total{};
    for (const auto& segment : flight.path) total = m::add(total, segment.length).value();
    return total;
}
void seeded_spatial_offsets() {
    auto profile = rocket_profile(); profile.curve_distance = q(50);
    profile.straight_distance = {}; profile.curve_offset = q(20);
    auto projectile = launch(profile, at(200));
    t::CombatRandom random(7, 9, 1, t::rocket_path_slot), expected(7, 9, 1, t::rocket_path_slot);
    d::RocketPathWork work;
    expect(static_cast<bool>(d::prepare_rocket_path(projectile, &random, &work)), "RFL-03: seeded route constructs");
    if (!projectile.flight || projectile.flight->path.size() != 4) {
        expect(false, "RFL-03: ceil(200 / 55) supplies four segments"); return;
    }
    const auto inverse_root_two = m::divide(q(1), m::sqrt(q(2)).value()).value();
    for (std::size_t i = 1; i < 4; ++i) {
        const auto turn = Fixed::from_raw(expected.uniform(0, static_cast<std::uint32_t>(Fixed::scale - 1)));
        const auto radius = m::multiply(q(20), Fixed::from_raw(expected.uniform(0, static_cast<std::uint32_t>(Fixed::scale)))).value();
        // The sourced midpoint axes for this X segment are (0,-1,1) and (0,-1,-1).
        const auto cosine = m::cos_turn(turn), sine = m::sin_turn(turn);
        const auto y = m::multiply(m::add(m::multiply(Fixed::from_raw(-inverse_root_two.raw()), cosine).value(),
            m::multiply(Fixed::from_raw(-inverse_root_two.raw()), sine).value()).value(), radius).value();
        const auto z = m::multiply(m::add(m::multiply(inverse_root_two, cosine).value(),
            m::multiply(Fixed::from_raw(-inverse_root_two.raw()), sine).value()).value(), radius).value();
        const auto& control = projectile.flight->path[i].a;
        expect(control.x == q(static_cast<std::int64_t>(i) * 50)
            && std::llabs(control.y.raw() - y.raw()) < 100 && std::llabs(control.z.raw() - z.raw()) < 100,
            "RFL-03: interior controls pin angle-before-radius uniform draws and spatial axes");
    }
    expect(random.draws() == 6 && work.control_points == 5 && work.arc_visits <= d::rocket_arc_visit_limit,
        "RFL-03/04: two draws per interior point, bounded arc work");
    expect(projectile.flight->path.front().a == at(0)
        && m::length(minus(spline_end(*projectile.flight), at(200))).value().raw() < 100,
        "RFL-03: endpoints have no random offsets");
    const auto cursor = random.draws();
    expect(static_cast<bool>(d::prepare_rocket_path(projectile, &random)) && random.draws() == cursor,
        "RFL-01/03: retained construction consumes no second set of draws");

    profile.straight_distance = q(200);
    projectile = launch(profile, at(400)); projectile.speed = q(32); projectile.step = at(32);
    t::CombatRandom scaled_random(7, 9, 1, t::rocket_path_slot);
    expect(static_cast<bool>(d::prepare_rocket_path(projectile, &scaled_random)), "RFL-02/03: initial/main scales construct");
    if (!projectile.flight || projectile.flight->path.size() != 8) {
        expect(false, "RFL-03: initial and main each supply four segments"); return;
    }
    for (std::size_t i = 1; i < 8; ++i) {
        if (i == 4) { expect(projectile.flight->path[i].a == at(200), "RFL-03: shared initial/main endpoint stays exact"); continue; }
        const auto& control = projectile.flight->path[i].a;
        const auto radius = m::length(m::Vec3{{}, control.y, control.z}).value();
        expect(radius.raw() <= q(i < 4 ? 2 : 20).raw() + 100, "RFL-03: initial offset is one tenth of the main offset");
    }
    expect(scaled_random.draws() == 12, "RFL-03: initial draws precede main draws without endpoint draws");
    profile.curve_offset = {}; projectile = launch(profile, at(400));
    t::CombatRandom stock(7, 9, 1, t::rocket_path_slot);
    expect(static_cast<bool>(d::prepare_rocket_path(projectile, &stock)) && stock.draws() == 0,
        "RFL-03: stock zero-offset construction consumes no draws");
}

void extension_and_construction_states() {
    auto profile = rocket_profile(); profile.target_radius = false;
    profile.authored_distance = q(10); profile.straight_distance = {};
    profile.curve_distance = Fixed::from_raw(Fixed::scale / 2);
    auto projectile = launch(profile, at(2));
    d::RocketPathWork work;
    expect(static_cast<bool>(d::prepare_rocket_path(projectile, nullptr, &work)), "RFL-05: short sampled-tangent extension constructs");
    expect(work.tangent_samples == 2 && projectile.flight->profile.authored_distance == q(10)
        && std::llabs(spline_end(*projectile.flight).x.raw() - q(6).raw()) < 100,
        "RFL-04/05: unnormalized half-unit sample delta yields endpoint 6, independent of allowance 10");
    const auto total = path_length(*projectile.flight);
    expect(d::rocket_point(*projectile.flight, total) && !d::rocket_point(*projectile.flight, total).value(),
        "RFL-04/05: equality at an extended endpoint exhausts lookup");
    d::CollectionTrees collidables; d::CombatWorld world; d::ProjectileScratch scratch;
    world.projectile_collection = &collidables;
    auto step = d::step_projectile(world, projectile, scratch);
    expect(step && step.value().expired && step.value().projectile.position == projectile.position
        && step.value().expiry_reason == t::ProjectileExpiryReason::rocket_path_exhausted && scratch.exact_count == 0,
        "RFL-06: exhausted extension terminates at current pose without collision or endpoint snap");

    profile.curve_offset = Fixed::from_raw(Fixed::scale / 100);
    auto bowed = launch(profile, at(2)); t::CombatRandom offset_random(7, 9, 1, t::rocket_path_slot);
    expect(static_cast<bool>(d::prepare_rocket_path(bowed, &offset_random, &work)), "RFL-05: post-aim offset constructs");
    if (bowed.flight->path.size() == 12) {
        const auto endpoint = spline_end(*bowed.flight);
        for (std::int64_t i = 1; i < 8; ++i) {
            const auto delta = minus(endpoint, at(2));
            const auto fraction = Fixed::from_raw(Fixed::scale * i / 8);
            const auto centre = plus(at(2), {m::multiply(delta.x, fraction).value(),
                m::multiply(delta.y, fraction).value(), m::multiply(delta.z, fraction).value()});
            const auto deviation = m::length(minus(bowed.flight->path[static_cast<std::size_t>(4 + i)].a, centre)).value();
            expect(deviation.raw() > 0 && deviation <= Fixed::from_raw(Fixed::scale * 3 / 1000 + 100),
                "RFL-03/05: post-aim interior offsets use three tenths of authored amplitude");
        }
        expect(offset_random.draws() == 20, "RFL-03/05: post-aim draws follow the six main draws, omitting all endpoints");
    } else expect(false, "RFL-05: nonzero short extension remains untrimmed at twelve segments");
    profile.curve_offset = {};

    // With roughly 45-unit segments the final sample delta is roughly nine: extension overshoots
    // allowance 400, then its selected spatial point becomes the rebuilt endpoint.
    profile.authored_distance = q(400); profile.curve_distance = q(50);
    projectile = launch(profile, at(220, 0, 40));
    expect(static_cast<bool>(d::prepare_rocket_path(projectile, nullptr, &work)), "RFL-05: overshooting extension trims and rebuilds");
    // Independent natural-cubic reference: the allowance selects segment 8 at
    // its arc fraction, yielding (393.5618028184, 0, 71.5566914215), not a
    // point whose distance from the origin is 400. Bound Q24 parameter
    // rounding through the rebuilt cut segment's derivative below.
    const m::Vec3 cutpoint{Fixed::from_raw(6602871375LL), {}, Fixed::from_raw(1200522068LL)};
    expect(!projectile.flight->path.empty() && projectile.flight->path.size() < work.control_points - 1
        && projectile.flight->aim == at(220, 0, 40)
        && spline_end(*projectile.flight).z > projectile.flight->aim.z,
        "RFL-01/05: trimming retains spatial aim and a post-aim endpoint");
    if (!projectile.flight->path.empty()) {
        const auto trimmed_total = path_length(*projectile.flight);
        const auto exact = d::rocket_point(*projectile.flight, trimmed_total);
        const auto below = d::rocket_point(*projectile.flight, Fixed::from_raw(trimmed_total.raw() - 1));
        const auto& last = projectile.flight->path.back();
        const auto norm1 = [](const m::Vec3& value) {
            return std::llabs(value.x.raw()) + std::llabs(value.y.raw()) + std::llabs(value.z.raw());
        };
        // At one raw distance unit below the endpoint, t differs by at most
        // one Q24 unit here. The cubic derivative bounds displacement; allow
        // another unit plus 32 raw units for fixed Horner evaluation rounding.
        const auto tolerance = 2 * (norm1(last.b) + 2 * norm1(last.c) + 3 * norm1(last.d)) / Fixed::scale + 32;
        // Add 1e-4 units for accumulated arc tolerance to the same derivative
        // bound. Arc inversion's roughly 0.0141-unit offset is far outside it.
        const auto cut_tolerance = tolerance + (Fixed::scale + 9999) / 10000;
        expect(100 * cut_tolerance < m::length(cutpoint).value().raw() - q(400).raw(),
            "RFL-04/05: cutpoint tolerance rejects arc inversion by more than one hundred times");
        expect(m::length(minus(spline_end(*projectile.flight), cutpoint)).value().raw() <= cut_tolerance,
            "RFL-04/05: derivative-bounded cutpoint rejects arc inversion");
        expect(exact && !exact.value() && below && below.value()
            && m::length(minus(*below.value(), spline_end(*projectile.flight))).value().raw() <= tolerance,
            "RFL-04/05: trimmed whole-path equality exhausts, with fractional lookup approaching its rebuilt endpoint");
    }
    profile.authored_distance = q(1);
    projectile = launch(profile, at(220));
    expect(static_cast<bool>(d::prepare_rocket_path(projectile, nullptr, &work))
        && projectile.flight->path_initialized && projectile.flight->path.empty(),
        "RFL-05/06: backwards extension and early trimming retain an unevaluable path, not construction failure");

    profile.target_radius = true; profile.curve_offset = q(1);
    projectile = launch(profile, at(220));
    const auto unbuilt = *projectile.flight;
    expect(!d::prepare_rocket_path(projectile) && *projectile.flight == unbuilt,
        "RFL-06: failed construction is a diagnostic and leaves no initialized route");
    profile.curve_distance = Fixed::from_raw(1);
    projectile = launch(profile, at(220)); t::CombatRandom random(7, 9, 1, t::rocket_path_slot);
    expect(!d::prepare_rocket_path(projectile, &random, &work) && !projectile.flight->path_initialized
        && projectile.flight->path.empty() && random.draws() == 0 && work.control_points <= d::rocket_point_limit,
        "RFL-03/06: pathological subdivision fails before draws or canonical path commit");
    auto repeated = projectile; d::RocketPathWork repeated_work;
    t::CombatRandom repeated_random(7, 9, 1, t::rocket_path_slot);
    expect(!d::prepare_rocket_path(repeated, &repeated_random, &repeated_work)
        && repeated_work.control_points == work.control_points && repeated_work.arc_visits == work.arc_visits,
        "RFL-03/04: failed construction has repeatable deterministic work bounds");
    profile = rocket_profile(); profile.straight_distance = {};
    projectile = launch(profile, at(1));
    expect(static_cast<bool>(d::prepare_rocket_path(projectile)) && projectile.flight->path_initialized
        && projectile.flight->path.empty(), "RFL-06: an unevaluable retained route is distinct from failed construction");
}

void seeded_worker_and_target_independence() {
    auto profile = rocket_profile(); profile.curve_offset = q(180);
    profile.curve_distance = q(400); profile.straight_distance = q(120);
    const auto original = launch(profile, at(1800, 200, 400));
    d::CollectionTrees collidables;
    d::CombatWorld world; world.seed = 717; world.frame = 19; world.projectile_collection = &collidables;
    d::CombatUnit target; target.id = 2; target.position = at(0, 9000, -1000); world.units.push_back(target);
    auto moved_world = world; moved_world.units[0].position = at(-8000, 0, 9000);
    d::ProjectileScratch first_scratch, moved_scratch;
    const auto first = d::step_projectile(world, original, first_scratch);
    const auto moved = d::step_projectile(moved_world, original, moved_scratch);
    expect(first && moved && first.value().projectile == moved.value().projectile,
        "RFL-01/03: retained aim height and seeded trajectory ignore current target movement");
    if (!first) return;
    expect(first.value().projectile.flight->aim == at(1800, 200, 400)
        && first.value().projectile.position.z.raw() != 0, "RFL-01: nonzero route retains and follows aim height");
    std::vector<std::uint8_t> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        std::vector<t::Projectile> flying(16, original);
        std::vector<std::uint8_t> succeeded(16);
        const auto ran = pool.execute_phase("rocket-contract", 16, [&](const std::size_t index) {
            auto projectile = original; projectile.id = index + 1;
            auto local_world = world;
            if (index % 2 == 1) projectile.flight->profile.target_radius = false;
            d::ProjectileScratch scratch;
            auto result = d::step_projectile(local_world, projectile, scratch);
            if (result) { flying[index] = result.value().projectile; succeeded[index] = 1; }
        });
        std::vector<std::uint8_t> bytes;
        for (std::size_t index = 0; index < flying.size(); ++index) {
            expect(succeeded[index] == 1, "RFL-01..06: partitioned seeded/extended flight succeeds");
            d::append_projectile(bytes, flying[index]);
        }
        expect(static_cast<bool>(ran), "RFL: threaded flight phase succeeds");
        if (workers == 1) reference = bytes;
        else expect(bytes == reference, "RFL-01..06: seeded and extended canonical flight bytes agree on 1/2/4/8 workers");
    }
}
// WAD-37/40: equality reveals a delayed projectile; the next frame begins movement.
void appearance_delay_contract() {
    for (const std::uint32_t delay : {15U, 30U}) {
        t::CombatEvent event{100, t::CombatEventKind::weapon_fired, 1, 0, 2, t::no_hardpoint, {}, at(500)};
        t::ShotProfile shot; shot.speed = q(7); shot.max_travel = q(1000); shot.appearance_delay_frames = delay;
        auto launched = d::launch_projectile(event, shot, 1, true, 1, {});
        expect(static_cast<bool>(launched), "MC-07: delayed projectile launches");
        if (!launched) continue;
        auto projectile = launched.value();
        expect(projectile.muzzle_delay_until == 100 + delay, "MC-07: authored frames define expiry");
        d::CollectionTrees collidables;
        d::CombatWorld world; d::ProjectileScratch scratch;
        world.projectile_collection = &collidables;
        for (std::uint64_t frame = 101; frame <= 100 + delay; ++frame) {
            world.frame = frame;
            auto step = d::step_projectile(world, projectile, scratch);
            expect(step && !step.value().hit && !step.value().expired && step.value().projectile.position == m::Vec3{}
                && step.value().projectile.travelled.raw() == 0, "MC-07: no delayed movement or collision through equality");
            if (step) projectile = step.value().projectile;
        }
        world.frame = 101 + delay;
        auto moved = d::step_projectile(world, projectile, scratch);
        expect(moved && moved.value().projectile.position.x.raw() > 0,
            "MC-07: first movement follows expiry by one frame");
        auto zero = shot; zero.appearance_delay_frames = 0;
        auto ordinary = d::launch_projectile(event, zero, 1, true, 1, {});
        expect(ordinary && ordinary.value().muzzle_delay_until == 0, "MC-07: zero delay carries no state extension");
        std::vector<std::uint8_t> delayed_bytes, ordinary_bytes;
        d::append_projectile(delayed_bytes, launched.value()); d::append_projectile(ordinary_bytes, ordinary.value());
        expect(delayed_bytes.size() == ordinary_bytes.size() + 8, "MC-07: positive PROJ delay appends only its expiry");
    }
}
void defence_flight_contracts() {
    d::CombatWorld world;
    d::CollectionTrees collidables; world.projectile_collection = &collidables;
    std::vector<t::SnapshotPlayer> relationships(3);
    for (std::size_t i = 0; i < relationships.size(); ++i) {
        relationships[i].player_id = static_cast<t::PlayerId>(i + 1);
        relationships[i].team_id = static_cast<t::TeamId>(i + 1);
    }
    world.relationships = relationships;
    d::ProjectileScratch scratch;
    t::CombatEvent event; event.shooter = 1; event.target = 2; event.target_hardpoint = t::no_hardpoint;
    event.origin = at(5); event.aim = at(100);
    t::ShotProfile profile; profile.speed = q(2); profile.max_travel = q(1000); profile.turn_rate = q(90);
    auto launched = d::launch_projectile(event, profile, 1, true, 1, {});
    expect(static_cast<bool>(launched), "WPJ-09: direct projectile launches");
    if (!launched) return;
    auto original = launched.value();
    d::ProjectileDefenceSource source;
    source.id = 50; source.owner = 2; source.position = at(10); source.adjusted_position = at(10);
    source.passive_shield = true; source.passive_radius = q(5);
    const auto run = [&](const t::Projectile& projectile, const std::vector<d::ProjectileDefenceSource>& sources) {
        expect(static_cast<bool>(world.projectile_defences.rebuild(sources)), "WPJ-17: typed registry builds");
        auto stepped = d::step_projectile(world, projectile, scratch);
        expect(static_cast<bool>(stepped), "WPJ-17: defence flight succeeds");
        return stepped ? stepped.value().projectile : projectile;
    };
    expect(run(original, {source}).position.x > original.position.x,
        "WPJ-17: direct radius equality remains outside (strict)");
    source.passive_radius = q(6);
    auto inside = run(original, {source});
    expect(inside.position.y > original.position.y && inside.position.x <= original.position.x,
        "WPJ-09/17: passive source overrides direct facing under turn limit");
    source.owner = 1;
    expect(run(original, {source}).step == original.step, "WPJ-17: allied source is skipped");
    source.owner = 2; relationships[1].team_id = 1;
    expect(run(original, {source}).step == original.step, "WPJ-17: another player's allied source is skipped");
    relationships[1].neutral = true;
    expect(run(original, {source}).position.y > original.position.y, "WPJ-17: a nonallied neutral source is eligible");
    relationships[1].neutral = false; relationships[1].team_id = 2;
    source.owner = 2; source.adjusted_position = at(10, 0, 20);
    expect(run(original, {source}).step == original.step, "WPJ-17: direct enclosure uses adjusted source height");
    source.adjusted_position = at(10, 0, 3); source.passive_radius = q(10);
    original.turn_rate = q(3);
    auto limited = run(original, {source});
    expect(limited.yaw == q(3) && limited.pitch == q(3),
        "WPJ-17: yaw and pitch each consume their independent turn limit");
    source.adjusted_position = original.position;
    original.step = at(-2); original.yaw = q(180); original.turn_rate = q(180);
    expect(run(original, {source}).step.x > Fixed{}, "WPJ-17: exact coincidence chooses world +X");
    original = launched.value();
    source.adjusted_position = at(10); source.passive_radius = q(20);
    auto later = source; later.id = 2; later.position = at(4); later.adjusted_position = at(4);
    expect(run(original, {source, later}).position.y > original.position.y,
        "WPJ-17: registration order wins over source ID and distance");
    source.passive_shield = false; source.active_shield = true;
    source.passive_radius = q(1); source.shield_radius = q(20);
    expect(run(original, {source}).position.y > original.position.y,
        "WPJ-17: active shield uses its ability radius");
    source.active_shield = false; source.passive_shield = true;
    expect(run(original, {source}).position.y > original.position.y,
        "WPJ-17: declared shield ability radius overrides passive radius even when inactive");
    source.passive_shield = false;
    expect(run(original, {source}).step == original.step, "WPJ-17: inactive unjammed nonpassive source is ignored");
    source.active_shield = false; source.sensor_jammed = true; source.jamming_radius = q(1);
    expect(run(original, {source}).step == original.step, "WPJ-17: jammer ability radius overrides shield data");
    source.jamming_radius = q(20);
    expect(run(original, {source}).position.y > original.position.y, "WPJ-17: jammed source deflects direct flight");
    profile.homing = true; profile.turn_rate = q(3);
    launched = d::launch_projectile(event, profile, 1, true, 3, {});
    d::CombatUnit target; target.id = 2; target.position = at(100, 100); target.sensor_jammed = true;
    world.units = {target};
    const auto jammed = run(launched.value(), {});
    expect(jammed.step == launched.value().step && jammed.locked,
        "WPJ-10: jammed target keeps direct flight without dropping the lock");
    world.units[0].sensor_jammed = false;
    expect(run(launched.value(), {}).yaw == q(3), "WPJ-10: unjammed target resumes pursuit");
    source.sensor_jammed = false; source.passive_shield = true; source.shield_radius.reset();
    source.passive_radius = q(20); source.adjusted_position = at(0);
    expect(run(launched.value(), {source}).yaw == Fixed{}, "WPJ-17: missile shield overrides pursuit before one turn");
    // A large far-away population must not become a per-projectile linear source scan.
    std::vector<d::ProjectileDefenceSource> many{source};
    for (std::uint64_t i = 1; i < 2000; ++i) {
        auto far = source; far.id = i + 100; far.position = at(10000 + static_cast<std::int64_t>(i) * 10);
        far.adjusted_position = far.position; many.push_back(far);
    }
    expect(static_cast<bool>(world.projectile_defences.rebuild(many)), "WPJ-17: large source registry builds");
    std::uint64_t inspected = 0;
    expect(world.projectile_defences.candidates(event.origin, &inspected).size() == 1 && inspected < 50,
        "WPJ-17: spatial lookup bounds source work independently of far-away population");
    scratch.defence_inspected = 0; scratch.defence_candidates = 0;
    const auto bounded = d::step_projectile(world, launched.value(), scratch);
    expect(bounded && scratch.defence_inspected > 0 && scratch.defence_inspected < 50 && scratch.defence_candidates == 1,
        "WPJ-17: production flight uses spatial source lookup with bounded inspected/admitted work");
    std::vector<std::uint8_t> before, after;
    original = launched.value(); original.homing = false; original.turn_rate = {};
    d::append_projectile(before, original); original.turn_rate = q(3); d::append_projectile(after, original);
    expect(after.size() == before.size() + 24 && after != before, "WPJ-17: direct turn rate/facing are conditional hashed state");
    before = after; after.clear(); original.yaw = q(19); d::append_projectile(after, original);
    expect(before != after, "WPJ-17: retained direct yaw participates in the state hash");
    before = after; after.clear(); original.pitch = q(7); d::append_projectile(after, original);
    expect(before != after, "WPJ-17: retained direct pitch participates in the state hash");
}

void external_redirect_contracts() {
    t::CombatEvent event; event.shooter = 1; event.target_hardpoint = 5; event.origin = at(20); event.aim = at(100);
    t::ShotProfile profile; profile.speed = q(5); profile.max_travel = q(1000); profile.damage = q(2); profile.damage_type = 4;
    auto launched = d::launch_projectile(event, profile, 1, true, 1, {});
    if (!launched) { expect(false, "WPJ-42: fixture launches"); return; }
    auto original = launched.value(); original.damage = q(99); original.damage_type = 7;
    original.speed = q(2); original.travelled = q(50);
    d::CombatUnit source; source.id = 8; source.owner = 2;
    d::ProjectileRedirect request; request.new_id = 2; request.source = &source;
    auto reversed = d::redirect_projectile(original, profile, request);
    expect(reversed && reversed.value().id == 2 && reversed.value().shooter == 8 && reversed.value().owner == 2
        && reversed.value().damage == q(99) && reversed.value().damage_type == 7 && !reversed.value().internal_damage_misc,
        "WPJ-42: new projectile changes ownership and preserves instance damage/type with redirected gate");
    expect(reversed && reversed.value().speed == q(5) && reversed.value().travelled == Fixed{}
        && reversed.value().max_travel == q(1000) && reversed.value().position == at(20)
        && reversed.value().target_hardpoint == t::no_hardpoint && reversed.value().step.x < Fixed{},
        "WPJ-42: redirect resets travel/speed/allowance, clears hardpoint and reverses facing");
    request.yaw_spread_draw = q(10); request.add_pitch = true; request.pitch_draw = q(-60);
    auto spread = d::redirect_projectile(original, profile, request);
    expect(spread && spread.value().yaw == q(190) && spread.value().pitch == q(-60),
        "WPJ-42: caller-supplied yaw and conditional pitch draws set fresh facing");
    t::CombatProfile target_profile; target_profile.ranged_target_z_adjust = q(10);
    d::CombatUnit target; target.id = 6; target.position = at(100); target.previous_position = at(100, -1); target.profile = &target_profile;
    request.target = &target;
    auto retargeted = d::redirect_projectile(original, profile, request);
    expect(retargeted && retargeted.value().target == 6 && retargeted.value().step.y > Fixed{}
        && retargeted.value().step.z > Fixed{}, "WPJ-42: replacement target uses extrapolated adjusted position");
    profile.flight = rocket_profile(); request.target = nullptr; request.yaw_spread_draw = {}; request.add_pitch = false;
    auto rocket = d::redirect_projectile(original, profile, request);
    expect(rocket && rocket.value().flight && rocket.value().flight->aim == at(-280)
        && original.max_travel == q(300), "WPJ-42: rocket gets a 300-unit planar endpoint and old allowance write");
}

void rocket_defence_contracts() {
    d::CombatWorld world; d::CollectionTrees collidables; world.projectile_collection = &collidables;
    const std::vector<t::SnapshotPlayer> relationships{{1, 1, false}, {2, 2, false}};
    world.relationships = relationships; d::ProjectileScratch scratch;
    auto profile = rocket_profile(); profile.authored_distance = q(500); profile.curve_distance = q(10);
    auto original = launch(profile, at(500), q(900)); original.owner = 1;
    const auto linear = [](t::Projectile& projectile) {
        projectile.flight->path_initialized = true;
        // A power-of-two arc length makes the boundary point exactly 25 in Q24.
        projectile.flight->path = {{at(0), at(512), {}, {}, q(512)}};
        projectile.position = at(0); projectile.step = at(25);
    };
    linear(original);
    d::ProjectileDefenceSource source;
    source.id = 3; source.owner = 2; source.position = at(40); source.adjusted_position = at(40, 0, 1000);
    source.passive_shield = true; source.passive_radius = q(15);
    const auto run = [&](const t::Projectile& projectile, const std::vector<d::ProjectileDefenceSource>& sources) {
        expect(static_cast<bool>(world.projectile_defences.rebuild(sources)), "RFL-07: rocket registry builds");
        const auto result = d::step_projectile(world, projectile, scratch);
        expect(static_cast<bool>(result), "RFL-07: detour flight succeeds");
        return result ? result.value().projectile : projectile;
    };
    auto boundary = run(original, {source});
    expect(boundary.flight->shield_redirected && boundary.position == at(25)
        && boundary.flight->path_distance == q(25) && boundary.target == original.target
        && boundary.flight->shield_allowance == q(500),
        "RFL-07: next radius equality enters using unadjusted source; failed ray retains route/cursor/target and marks redirect");
    source.passive_radius = Fixed::from_raw(q(15).raw() - 1);
    expect(!run(original, {source}).flight->shield_redirected, "RFL-07: a next point outside the radius does not enter");
    source.position = at(20); source.passive_radius = q(100);
    expect(!run(original, {source}).flight->shield_redirected, "RFL-07: current point already strictly inside does not enter");
    source.position = at(100, 20); source.passive_radius = q(80); source.owner = 1;
    expect(!run(original, {source}).flight->shield_redirected, "RFL-07: allied rocket shield is skipped");
    source.owner = 2;
    auto detour = run(original, {source});
    expect(detour.flight->shield_redirected && detour.position == at(25)
        && detour.flight->path_distance == Fixed{} && detour.flight->shield_allowance == q(475)
        && detour.flight->profile.authored_distance == q(500)
        && detour.target == eawr::sim::invalid_entity_id && !detour.locked,
        "RFL-07: successful detour retains this frame's point, resets cursor/tracking and subtracts consumed authored allowance");
    expect(detour.flight->path != original.flight->path && detour.flight->path.size() > 3,
        "RFL-07: detour rebuilds the cubic through spherical midpoint controls");
    bool bowed = false;
    for (const auto& segment : detour.flight->path) bowed = bowed || segment.a.y < Fixed{};
    expect(bowed, "RFL-07: off-axis spherical detour bows away from the shield centre");
    source.position = at(80, 60); source.passive_radius = q(100);
    expect(run(original, {source}).flight->shield_redirected, "RFL-07: current radius equality is an inclusive entry");
    source.position = at(100, 20); source.passive_radius = q(80);
    auto repeated = detour; linear(repeated); repeated.flight->path_distance = q(100);
    // Keep this frame's point at 25 with 125 units consumed on the prior route.
    repeated.flight->path[0].a = at(-100);
    const auto again = run(repeated, {source});
    expect(again.flight->shield_allowance == q(350) && again.flight->profile.authored_distance == q(500),
        "RFL-07: repeated detour reduces the retained allowance while preserving the type's original trim limit");
    std::vector<std::uint8_t> before, after;
    auto unmarked = boundary; unmarked.flight->shield_redirected = false;
    d::append_projectile(before, unmarked); d::append_projectile(after, boundary);
    expect(after.size() == before.size() + 8 && after != before, "RFL-07: sticky marker conditionally hashes remaining allowance");
    before = after; after.clear(); boundary.flight->shield_allowance = q(499); d::append_projectile(after, boundary);
    expect(before != after, "RFL-07: remaining allowance affects state hash");
}
} // namespace
int main() {
    defence_flight_contracts(); external_redirect_contracts(); rocket_defence_contracts();
    impact_suppresses_expiry();
    appearance_delay_contract();
    endpoint_and_retained_aim(); cubic_height_and_boundary(); default_endpoints_and_lifetime(); unsupported_and_encoding();
    seeded_spatial_offsets(); extension_and_construction_states(); seeded_worker_and_target_independence();
    if (failures) return 1;
    std::cout << "rocket endpoint contracts passed (WAD-04, RFL-01..08)\n";
    return 0;
}
