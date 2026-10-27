#include "../support/check.hpp"
#include "../support/env.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"
#include "../../src/sim/tactical/collection.hpp"
#include "../../src/sim/tactical/rocket_internal.hpp"
#include "../../src/sim/tactical/motion_internal.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace t = eawr::sim::tactical;
namespace d = t::detail;
namespace m = eawr::sim::math;
using m::Fixed;
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
Fixed q(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }
m::Vec3 at(const std::int64_t x, const std::int64_t y = 0, const std::int64_t z = 0) { return {q(x), q(y), q(z)}; }
m::Vec3 plus(const m::Vec3& a, const m::Vec3& b) {
    return {m::add(a.x, b.x).value(), m::add(a.y, b.y).value(), m::add(a.z, b.z).value()};
}
m::Vec3 minus(const m::Vec3& a, const m::Vec3& b) {
    return {m::subtract(a.x, b.x).value(), m::subtract(a.y, b.y).value(), m::subtract(a.z, b.z).value()};
}
void expect(const bool value, const std::string_view message) {
    if (!value) local_checker.check_before_print(value, message, std::cerr, "FAIL: ");
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

void oversized_flight_allowance() {
    const auto parsed = Fixed::from_decimal("549755813887.999999940395355224609375");
    expect(parsed && parsed.value().raw() == std::numeric_limits<std::int64_t>::max(),
        "RFL-16: largest positive Q24 authored allowance parses exactly");
    if (!parsed) return;
    std::vector<std::uint8_t> reference;
    std::vector<std::string> reference_diagnostics;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        std::vector<t::Projectile> flying;
        for (std::size_t index = 0; index < 16; ++index) {
            auto flight = rocket_profile();
            flight.authored_distance = index % 2 == 0 ? q(9999999) : parsed.value();
            flight.target_radius = index % 4 < 2;
            // Target-radius cases exercise the reported nonzero-offset geometry.
            // Extension cases keep storage small while retaining a ten-unit sample delta.
            flight.curve_distance = flight.target_radius ? q(450) : q(200000);
            flight.curve_offset = flight.target_radius ? q(90) : Fixed{};
            flight.straight_distance = q(250);
            auto projectile = launch(flight, at(1800), q(9999999));
            projectile.id = index + 1;
            flying.push_back(projectile);
        }
        const auto original = flying;
        std::vector<std::uint8_t> terminal(16), prepared(16), atomic(16), succeeded(16, 1), moved(16);
        std::vector<unsigned> diagnostic_count(16);
        std::vector<std::string> diagnostics(16);
        d::CollectionTrees collidables;
        d::CombatWorld world; world.seed = 5062; world.projectile_collection = &collidables;
        for (std::uint64_t frame = 1; frame <= 8; ++frame) {
            world.frame = frame;
            const auto ran = pool.execute_phase("rocket-large-allowance", flying.size(), [&](const std::size_t index) {
                if (terminal[index]) return;
                if (frame == 1) {
                    auto candidate = flying[index];
                    t::CombatRandom random(world.seed, frame, candidate.id, t::rocket_path_slot);
                    const auto built = d::prepare_rocket_path(candidate, &random);
                    prepared[index] = built ? 1 : 0;
                    atomic[index] = !built && candidate == flying[index] ? 1 : 0;
                }
                d::ProjectileScratch scratch;
                const auto result = d::step_projectile(world, flying[index], scratch);
                if (!result) { succeeded[index] = 0; terminal[index] = 1; return; }
                const auto& step = result.value();
                if (step.projectile.position != flying[index].position) moved[index] = 1;
                if (step.path_diagnostic) {
                    ++diagnostic_count[index]; diagnostics[index] += step.path_diagnostic->message;
                }
                flying[index] = step.projectile;
                terminal[index] = step.expired ? 1 : 0;
            });
            expect(static_cast<bool>(ran), "RFL-16: partitioned oversized flight completes");
        }
        std::vector<std::uint8_t> bytes;
        for (std::size_t index = 0; index < flying.size(); ++index) {
            const bool overflow = index % 4 == 3;
            expect(succeeded[index] && (prepared[index] != 0) == !overflow,
                "RFL-16: huge extension and maximum target-radius allowance construct; actual overflow refuses");
            expect(flying[index].flight->profile.authored_distance == original[index].flight->profile.authored_distance,
                "RFL-16: authored allowance is preserved without clamping");
            if (overflow) {
                expect(atomic[index] && terminal[index] && !moved[index] && diagnostic_count[index] == 1
                    && !flying[index].flight->path_initialized
                    && diagnostics[index].find("rocket extension construction") != std::string::npos,
                    "RFL-09/16: actual extension overflow is atomic, terminal at current pose, diagnostic once");
            } else {
                expect(moved[index] && !terminal[index] && diagnostic_count[index] == 0,
                    "RFL-16: representable oversized profiles continue flying through later ticks");
            }
            d::append_projectile(bytes, flying[index]);
        }
        if (workers == 1) { reference = bytes; reference_diagnostics = diagnostics; }
        else expect(bytes == reference && diagnostics == reference_diagnostics,
            "RFL-09/16: large-allowance paths, overflow and diagnostics agree on 1/2/4/8 workers");
    }
    t::CombatTable table; table.profiles.emplace_back(); table.profiles.back().type_id = 1;
    t::WeaponProfile weapon; weapon.shot.emplace(); weapon.shot->speed = q(25);
    weapon.shot->flight = rocket_profile(); weapon.shot->flight->curve_distance = {};
    table.profiles.back().weapons.push_back(weapon);
    const auto invalid = t::validate_combat(table);
    expect(!invalid && invalid.error().message.find("object weapon") != std::string::npos
        && invalid.error().message.find("Projectile_Rocket_Curve_Distance must be positive") != std::string::npos
        && invalid.error().message.find("4294967295") == std::string::npos,
        "RFL-09/16: malformed subdivision reports the object weapon and actual tag, without a sentinel");
    table.profiles.back().weapons.back().hardpoint = 3;
    const auto invalid_hardpoint = t::validate_combat(table);
    expect(!invalid_hardpoint && invalid_hardpoint.error().message.find("hardpoint 3") != std::string::npos
        && invalid_hardpoint.error().message.find("Projectile_Rocket_Curve_Distance must be positive") != std::string::npos,
        "RFL-09/16: direct hardpoint profile errors identify their slot and failed input");
    auto zero = rocket_profile(); zero.authored_distance = {};
    expect(d::valid_flight(zero), "RFL-16: zero allowance keeps the existing accepted domain");
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
    expect(random.draws() == 6 && work.control_points == 5 && work.arc_visits > 0,
        "RFL-03/04: two draws per interior point with measured arc work");
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
    const d::RocketPathPolicy small{32};
    projectile = launch(profile, at(220)); t::CombatRandom random(7, 9, 1, t::rocket_path_slot);
    expect(!d::prepare_rocket_path(projectile, &random, &work, small) && !projectile.flight->path_initialized
        && projectile.flight->path.empty() && random.draws() == 0 && work.control_points == 1 && work.required_points > small.max_controls,
        "RFL-03/06: injected deterministic storage refusal precedes draws and canonical path commit");
    auto repeated = projectile; d::RocketPathWork repeated_work;
    t::CombatRandom repeated_random(7, 9, 1, t::rocket_path_slot);
    expect(!d::prepare_rocket_path(repeated, &repeated_random, &repeated_work, small)
        && repeated_work.control_points == work.control_points && repeated_work.arc_visits == work.arc_visits,
        "RFL-03/04: failed construction has repeatable deterministic work bounds");
    profile = rocket_profile(); profile.straight_distance = {};
    projectile = launch(profile, at(1));
    expect(static_cast<bool>(d::prepare_rocket_path(projectile)) && projectile.flight->path_initialized
        && projectile.flight->path.empty(), "RFL-06: an unevaluable retained route is distinct from failed construction");
}

// Independent unlimited recursive RFL-04 reference, with the same Q24 policy at
// a nonprogressing interval. No route-wide work/depth ceiling.
Fixed reference_arc(const t::RocketSegment& segment, const Fixed low, const Fixed high, std::size_t& visits) {
    using t::motion_detail::Calc;
    Calc calc;
    const auto scale = [&](const m::Vec3& v, const Fixed factor) {
        return m::Vec3{calc.mul(v.x, factor), calc.mul(v.y, factor), calc.mul(v.z, factor)};
    };
    const auto eval = [&](const Fixed value) {
        return plus(segment.a, scale(plus(segment.b, scale(plus(segment.c, scale(segment.d, value)), value)), value));
    };
    ++visits;
    const auto first = eval(low), last = eval(high);
    const auto chord = calc.length(minus(last, first));
    const auto tolerance = Fixed::from_raw(17);
    if (chord < tolerance) return chord;
    const auto mid = Fixed::from_raw(low.raw() + (high.raw() - low.raw()) / 2);
    const auto center = eval(mid);
    const auto halves = calc.add(calc.length(minus(center, first)), calc.length(minus(last, center)));
    if (halves.raw() == 0 || calc.abs(calc.sub(calc.div(chord, halves), q(1))) <= tolerance
        || calc.abs(calc.sub(chord, halves)) <= tolerance || mid == low || mid == high) return halves;
    const auto left = reference_arc(segment, low, mid, visits);
    const auto right = reference_arc(segment, mid, high, visits);
    expect(calc.ok(), "RFL-04: reference arithmetic remains representable");
    return calc.add(left, right);
}

void dynamic_route_contracts() {
    d::RocketPathWork numeric_work;
    const t::RocketSegment fast{{}, at(1000000), {}, {}, {}};
    const auto estimate = d::rocket_arc_length(fast, {}, Fixed::from_raw(1), numeric_work);
    expect(estimate && estimate.value().raw() == 1000000 && numeric_work.midpoint_estimates == 1,
        "RFL-04/09: Q24 nonprogress accepts available two-chord estimate with a diagnostic counter");
    auto profile = rocket_profile(); profile.straight_distance = {}; profile.curve_distance = q(1);
    const auto spacing = Fixed::from_raw(Fixed::scale * 11 / 10);
    for (const std::size_t count : {2047U, 2048U, 2049U, 8193U}) {
        auto projectile = launch(profile);
        projectile.flight->aim = {Fixed::from_raw(spacing.raw() * static_cast<std::int64_t>(count - 1)), {}, {}};
        d::RocketPathWork work;
        const auto built = d::prepare_rocket_path(projectile, nullptr, &work);
        expect(built && work.control_points == count && projectile.flight->path.size() + 1 == count,
            "RFL-03/09: full authored subdivision below, at and above former control ceiling");
        if (!built) continue;
        expect(m::length(minus(spline_end(*projectile.flight), projectile.flight->aim)).value().raw() <= 4,
            "RFL-03/04: dynamic natural cubic preserves exact endpoint");
        std::size_t visits{};
        for (const auto& segment : projectile.flight->path) {
            expect(segment.length == reference_arc(segment, {}, q(1), visits),
                "RFL-04: dynamic spline lengths equal unlimited recursive reference");
        }
        expect(visits == work.arc_visits, "RFL-04: exact visits count all segment evaluations");
    }
    auto repro = launch(profile, at(2252)); d::RocketPathWork work;
    expect(d::prepare_rocket_path(repro, nullptr, &work) && work.control_points == 2049,
        "RFL-09: formerly rejected 2252-unit spacing-one route constructs 2049 controls");
    d::CombatWorld world; d::CollectionTrees collidables; world.projectile_collection = &collidables;
    d::ProjectileScratch scratch;
    const auto moved = d::step_projectile(world, repro, scratch);
    expect(moved && !moved.value().expired && moved.value().projectile.position.x > repro.position.x,
        "RFL-09: old-boundary route advances instead of terminal fallback");
    profile.target_radius = false;
    auto extended = launch(profile, at(220));
    expect(d::prepare_rocket_path(extended, nullptr, &work) && work.control_points > 2048
        && work.tangent_samples == 2 && extended.flight->path.size() > 2048,
        "RFL-05/09: unnormalized extension crosses former ceiling before trim");

    // Many lightly curved controls make cumulative evaluation/extension/trim work
    // cross the former route-wide visit ceiling without an expensive deep curve.
    profile.curve_offset = Fixed::from_raw(1); profile.authored_distance = q(90000);
    auto curved = launch(profile, at(100000)); t::CombatRandom random(7, 9, 1, t::rocket_path_slot);
    const auto built = d::prepare_rocket_path(curved, &random, &work);
    expect(built && work.arc_visits > 131072 && work.control_points > 2048 && work.tangent_samples == 2,
        "RFL-04/05/09: curved extension and trim exceed former cumulative visit ceiling");
    if (built) {
        std::size_t visits{};
        for (const auto& segment : curved.flight->path) expect(
            segment.length == reference_arc(segment, {}, q(1), visits),
            "RFL-04/09: large curved trimmed path equals unlimited reference");
    }
}

void refusal_and_dynamic_workers() {
    auto profile = rocket_profile(); profile.straight_distance = {}; profile.curve_distance = q(1);
    const d::RocketPathPolicy small{32};
    d::CombatWorld world; d::CollectionTrees collidables; world.projectile_collection = &collidables;
    std::vector<std::uint8_t> reference;
    std::vector<std::string> reference_diagnostics;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        std::vector<t::Projectile> flying;
        for (std::size_t index = 0; index < 8; ++index) {
            auto projectile = launch(profile, at(index % 2 == 0 ? 2252 : 20)); projectile.id = index + 1;
            flying.push_back(projectile);
        }
        std::vector<std::string> diagnostics(flying.size());
        std::vector<std::uint8_t> terminal(flying.size());
        for (std::uint64_t frame = 1; frame <= 3; ++frame) {
            world.frame = frame;
            const auto ran = pool.execute_phase("rocket-dynamic", flying.size(), [&](const std::size_t index) {
                if (terminal[index]) return;
                d::ProjectileScratch scratch;
                // Only one refused projectile; the other long paths must advance.
                const auto stepped = d::step_projectile(world, flying[index], scratch, nullptr, index == 0 ? &small : nullptr);
                expect(static_cast<bool>(stepped), "RFL-09: refusal is a successful per-projectile terminal result");
                if (!stepped) return;
                const auto& result = stepped.value();
                if (index == 0) {
                    expect(result.expired && result.expiry_reason == t::ProjectileExpiryReason::rocket_path_exhausted
                        && result.projectile.position == flying[index].position && !result.projectile.flight->path_initialized
                        && !result.hit && scratch.candidate_count == 0 && result.path_diagnostic.has_value(),
                        "RFL-06/09: refusal retains current pose, no partial path or collision, one diagnostic");
                }
                if (index == 2) expect(!result.expired && result.projectile.position.x > flying[index].position.x,
                    "RFL-09: unrelated long projectile keeps advancing through later frames");
                if (result.path_diagnostic) diagnostics[index] += result.path_diagnostic->message;
                flying[index] = result.projectile;
                terminal[index] = result.expired ? 1 : 0;
            });
            expect(static_cast<bool>(ran), "RFL-09: partitioned dynamic/refused flight completes later frames");
        }
        std::vector<std::uint8_t> bytes;
        for (const auto& projectile : flying) d::append_projectile(bytes, projectile);
        if (workers == 1) { reference = bytes; reference_diagnostics = diagnostics; }
        else expect(bytes == reference && diagnostics == reference_diagnostics,
            "RFL-09: dynamic/refused canonical flight and diagnostic order agree on 1/2/4/8 workers");
    }
    auto invalid = launch(profile); invalid.flight->profile.curve_distance = {};
    d::ProjectileScratch scratch;
    const auto malformed = d::step_projectile(world, invalid, scratch);
    expect(!malformed && malformed.error().code == t::diagnostic_codes::invalid_setup,
        "RFL-09: malformed profile remains setup failure, separate from storage refusal");
    auto overflow = launch(profile); overflow.flight->aim.x = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    // Force an unrepresentable Q24 quotient before storage allocation. A spacing
    // of one whole unit instead requests a representable multi-terabyte route.
    overflow.flight->profile.curve_distance = Fixed::from_raw(1);
    const auto refused = d::step_projectile(world, overflow, scratch);
    expect(refused && refused.value().expired && refused.value().path_diagnostic
        && refused.value().projectile.position == overflow.position,
        "RFL-09: unrepresentable geometry expires only its projectile at current pose");
}

void refusal_session_case(const bool oversized_allowance) {
    t::TacticalSetup setup;
    setup.seed = 73;
    setup.players = {{1, 1, 1, t::player_flag_commandable}, {2, 2, 2, t::player_flag_commandable}};
    for (std::uint32_t id = 1; id <= 3; ++id) {
        t::UnitState unit; unit.entity_id = id; unit.type_id = id; unit.owner = id == 3 ? 2 : 1;
        unit.position = id == 3 ? at(100000) : at(0, static_cast<std::int64_t>(id) * 10);
        unit.rotation = m::identity_quat(); setup.units.push_back(unit);
    }
    t::CombatTable combat;
    combat.priority_sets.emplace_back(); combat.priority_sets.back().unlisted = q(1);
    for (std::uint32_t type = 1; type <= 3; ++type) {
        t::CombatProfile profile; profile.type_id = type; profile.category_bits = 1; profile.priority_set = 0;
        profile.max_attack_distance = q(200000);
        if (type < 3) {
            t::WeaponProfile weapon; weapon.range = q(200000);
            weapon.min_recharge_hundredths = 0; weapon.max_recharge_hundredths = 0;
            weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
            weapon.shot.emplace(); weapon.shot->speed = q(25); weapon.shot->max_travel = q(200000);
            auto flight = rocket_profile(); flight.straight_distance = {};
            // Count division itself is unrepresentable, before any large allocation.
            flight.curve_distance = type == 1 ? Fixed::from_raw(1) : q(500);
            if (oversized_allowance) {
                flight.authored_distance = type == 1
                    ? Fixed::from_raw(std::numeric_limits<std::int64_t>::max()) : q(9999999);
                flight.target_radius = type != 1;
                flight.straight_distance = q(250);
                flight.curve_distance = type == 1 ? q(200000) : q(450);
            }
            weapon.shot->flight = flight; profile.weapons.push_back(weapon);
        }
        combat.profiles.push_back(profile);
    }
    const std::vector<t::SensorProfile> sensors{{1, q(200000)}, {2, q(200000)}, {3, q(200000)}};
    t::DurabilityTable durability;
    durability.damage.emplace(); durability.damage->damage_types = 1; durability.damage->armor_types = 1;
    durability.damage->armor_mods = {q(1)};
    durability.damage->shield_recharge_frames = 1;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto created = t::TacticalSession::create(setup, sensors, durability, {}, std::nullopt, combat);
        expect(static_cast<bool>(created), "RFL-09: mixed refusal session accepts valid numeric profile domains");
        if (!created) continue;
        auto session = std::move(created).value(); eawr::platform::ThreadWorkerAdapter pool(workers);
        std::size_t diagnostics{}; bool survivor = false, stopped_source = false; std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 8; ++tick) {
            const auto stepped = session.step(pool);
            expect(static_cast<bool>(stepped), "RFL-09: one numerical refusal does not stop later battle ticks");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            for (const auto& entry : stepped.value().diagnostics) {
                if (entry.code == t::diagnostic_codes::resource_limit) {
                    ++diagnostics; hashes.push_back(entry.message);
                }
            }
            bool fired = false;
            for (const auto& projectile : session.projectiles()) {
                if (projectile.shooter == 2 && projectile.position.x > Fixed{}) survivor = true;
                fired = fired || projectile.shooter == 1;
            }
            // Isolate one refused shot, while the other source keeps firing.
            if (fired && !stopped_source) {
                expect(static_cast<bool>(session.stage_remove(1)), "RFL-09: remove source after its single shot");
                stopped_source = true;
            }
        }
        expect(diagnostics == 1 && survivor, "RFL-09: one diagnostic and unrelated projectile advancement in real session");
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "RFL-09: real session hashes and diagnostic order equal on 1/2/4/8 workers");
    }
}
void refusal_session_contract() {
    refusal_session_case(false);
    refusal_session_case(true); // RFL-16: overflowing extension and unrelated huge-allowance flight.
}

void dynamic_detour_contracts() {
    auto profile = rocket_profile(); profile.curve_distance = Fixed::from_raw(Fixed::scale / 10);
    profile.authored_distance = q(500);
    auto projectile = launch(profile, at(500));
    projectile.flight->path_initialized = true;
    projectile.flight->path = {{at(0), at(512), {}, {}, q(512)}};
    d::RocketPathWork work;
    const auto built = d::repath_rocket(projectile, at(100, 20), at(25), &work);
    expect(built && work.control_points > 2048 && projectile.flight->path.size() > 2048,
        "RFL-07/09: spherical plus straight detour exceeds former control ceiling");
    if (built) {
        projectile.flight->path_distance = q(25);
        const auto repeated = d::repath_rocket(projectile, at(100, 20), at(25), &work);
        expect(repeated && projectile.flight->shield_allowance == q(475) && work.control_points > 2048,
            "RFL-07/09: repeated dynamic detour preserves consumed allowance");
    }
    auto original = launch(profile, at(500)); const auto before = original;
    const d::RocketPathPolicy small{8};
    const auto refused = d::repath_rocket(original, at(100, 20), at(25), &work, small);
    expect(!refused && refused.error().code == t::diagnostic_codes::resource_limit && original == before,
        "RFL-07/09: refused detour leaves path, marker, allowance and tracking atomic");
    auto ray_profile = rocket_profile(); ray_profile.curve_distance = q(10);
    // A unit step normalizes exactly in Q24; a 25-unit step does not.
    const auto exact_step = at(1);
    t::motion_detail::Calc geometry;
    const auto heading_x = geometry.mul(exact_step.x, geometry.div(q(1), geometry.length(exact_step)));
    const auto lead_x = geometry.mul(heading_x, q(60));
    const auto ray_intersection = [&](const m::Vec3& source) {
        return geometry.neg(geometry.mul(q(2), geometry.mul(geometry.sub(lead_x, source.x), heading_x)));
    };
    auto ray = launch(ray_profile, at(500));
    auto source = at(60, 20); source.x = Fixed::from_raw(q(60).raw() + Fixed::scale / 4 - 1);
    expect(heading_x == q(1) && lead_x == q(60)
        && ray_intersection(source).raw() == Fixed::scale / 2 - 2,
        "RFL-07: outside-ray fixture lies exactly two Q24 raw units below the half-unit bound");
    expect(d::repath_rocket(ray, source, exact_step) && !ray.flight->path_initialized && ray.flight->shield_redirected,
        "RFL-07: ray immediately below half-unit bound retains old route");
    ray = launch(ray_profile, at(500)); source.x = Fixed::from_raw(source.x.raw() + 1);
    expect(ray_intersection(source).raw() == Fixed::scale / 2,
        "RFL-07: inclusive-ray fixture intersects at exactly half a unit in Q24");
    expect(d::repath_rocket(ray, source, exact_step) && ray.flight->path_initialized,
        "RFL-07: ray at inclusive half-unit bound constructs route");
    // A power-of-two radius also makes the opposed normalized seeds exact.
    const auto antipodal_source = at(92);
    const auto near_x = geometry.sub(lead_x, antipodal_source.x);
    const auto far_x = geometry.add(near_x, geometry.mul(heading_x, ray_intersection(antipodal_source)));
    const auto near_direction = geometry.mul(near_x, geometry.div(q(1), geometry.abs(near_x)));
    const auto far_direction = geometry.mul(far_x, geometry.div(q(1), geometry.abs(far_x)));
    expect(geometry.ok() && near_x == q(-32) && far_x == q(32)
        && near_direction == q(-1) && far_direction == q(1)
        && geometry.add(near_direction, far_direction) == Fixed{},
        "RFL-07/09: antipodal fixture has exact opposed Q24 seeds and a zero midpoint");
    expect(geometry.atan2_deg({}, q(-1)) == q(-180),
        "RFL-07/09: antipodal angle wraps to a negative half-turn before the angular stop");
    const auto antipodal = d::repath_rocket(original, antipodal_source, exact_step, &work);
    expect(!antipodal && antipodal.error().code == t::diagnostic_codes::resource_limit && original == before,
        "RFL-07/09: antipodal midpoint refuses atomically instead of looping");
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
        auto& flight = projectile.flight.value();
        flight.path_initialized = true;
        // A power-of-two arc length makes the boundary point exactly 25 in Q24.
        flight.path.clear();
        flight.path.push_back({at(0), at(512), {}, {}, q(512)});
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
// RFL-15 (#4986): one 24-missile unit-level salvo with a Victory-like muzzle layout (three four-bone
// batteries per flank, bone x axes 70 degrees off the bow, outward; research/victory-salvo-launch.md)
// at a target dead ahead. Each rocket leaves along its bone axis for the straight leg, then curves
// to the aim; without a launch direction (the old path) it leaves straight at the aim. Set
// EAWR_ROCKET_TRACE to print every frame's raw Q24 position and step for the path plot.
void unit_salvo_launch_axis() {
    const bool trace = eawr::testing::environment("EAWR_ROCKET_TRACE").has_value();
    const auto tenth = [](const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale / 10); };
    struct Battery { std::int64_t x0, y0, x1, y1; };
    // Model (x, y) in tenths, bow -y: four bones each from the first point to the second.
    const Battery batteries[6]{{726, -101, 766, 21}, {-726, -101, -766, 21}, {851, 289, 887, 402},
        {-851, 289, -892, 411}, {976, 681, 1015, 804}, {-977, 680, -1016, 804}};
    const auto port = m::normalize(m::Vec3{Fixed::from_raw(342 * Fixed::scale / 1000), Fixed::from_raw(940 * Fixed::scale / 1000), {}}).value();
    const auto starboard = m::Vec3{port.x, Fixed::from_raw(-port.y.raw()), {}};
    auto flight = rocket_profile();
    flight.curve_distance = q(800); flight.curve_offset = q(5); flight.straight_distance = q(400);
    const auto aim = at(3000);
    for (const bool launched : {false, true}) {
        d::CollectionTrees collidables;
        d::CombatWorld world; d::ProjectileScratch scratch;
        world.projectile_collection = &collidables;
        world.seed = 4986;
        std::vector<t::Projectile> salvo;
        std::vector<m::Vec3> axes;
        for (std::int64_t muzzle = 0; muzzle < 24; ++muzzle) {
            const auto& b = batteries[muzzle / 4];
            const auto k = muzzle % 4;
            const auto mx = b.x0 + (b.x1 - b.x0) * k / 3, my = b.y0 + (b.y1 - b.y0) * k / 3;
            t::CombatEvent event; event.shooter = 1; event.target = 2; event.aim = aim;
            event.origin = m::Vec3{tenth(-my), tenth(mx), {}}; // unit x = -model y, unit y = model x
            const auto axis = mx > 0 ? port : starboard;
            if (launched) event.launch = axis;
            t::ShotProfile shot; shot.speed = q(25); shot.max_travel = q(5000); shot.flight = flight;
            shot.turn_rate = q(25); // inert for a rocket's path (RFL-11)
            auto created = d::launch_projectile(event, shot, 1, true, static_cast<std::uint64_t>(muzzle + 1), {});
            expect(static_cast<bool>(created), "RFL-15: salvo rocket launches");
            if (!created) return;
            salvo.push_back(created.value());
            axes.push_back(axis);
        }
        for (std::size_t i = 0; i < salvo.size(); ++i) {
            const auto origin = salvo[i].position;
            Fixed widest = Fixed::from_raw(std::llabs(origin.y.raw())), nearest = q(100000), bend = q(100000);
            // RFL-12: the straight leg ends min(400, planar distance / 2) along the launch facing.
            const auto corner = plus(origin, m::Vec3{m::multiply(axes[i].x, q(400)).value(), m::multiply(axes[i].y, q(400)).value(), {}});
            m::Vec3 straight{};
            for (std::uint32_t frame = 0; frame < 400; ++frame) {
                world.frame = 100 + frame;
                auto step = d::step_projectile(world, salvo[i], scratch);
                expect(static_cast<bool>(step), "RFL-15: salvo rocket step succeeds");
                if (!step) return;
                salvo[i] = step.value().projectile;
                const auto& p = salvo[i].position;
                if (trace) std::cout << "TRACE " << (launched ? "after" : "before") << ' ' << i << ' ' << frame << ' '
                    << p.x.raw() << ' ' << p.y.raw() << ' ' << salvo[i].step.x.raw() << ' '
                    << salvo[i].step.y.raw() << '\n';
                widest = std::max(widest, Fixed::from_raw(std::llabs(p.y.raw())));
                nearest = std::min(nearest, m::length(minus(aim, p)).value());
                bend = std::min(bend, m::length(minus(corner, p)).value());
                if (frame == 7) straight = minus(p, origin);
                if (step.value().expired) break;
            }
            const auto along = m::divide(m::dot(straight, axes[i]).value(), m::length(straight).value()).value();
            if (launched) {
                expect(along > Fixed::from_raw(Fixed::scale * 85 / 100),
                    "RFL-15/RFL-12: the rocket departs along the muzzle bone's x axis (sideways)");
                expect(bend < q(15), "RFL-15/RFL-12: the straight leg ends 400 units along the muzzle bone's x axis");
                expect(widest > q(400), "RFL-15: the rocket swings out wide of its flank before it curves in");
            } else {
                expect(widest < q(115), "RFL-15: without a launch direction the rocket leaves at its aim");
            }
            expect(nearest < q(40), "RFL-15: every rocket still reaches its aim point");
        }
    }
}

int main() {
    const auto run = [](const char* name, void (*contract)()) {
        std::cout << "CASE " << name << std::endl;
        contract();
    };
    run("dynamic_route_contracts", dynamic_route_contracts);
    run("refusal_and_dynamic_workers", refusal_and_dynamic_workers);
    run("refusal_session_contract", refusal_session_contract);
    run("dynamic_detour_contracts", dynamic_detour_contracts);
    run("defence_flight_contracts", defence_flight_contracts);
    run("external_redirect_contracts", external_redirect_contracts);
    run("rocket_defence_contracts", rocket_defence_contracts);
    run("impact_suppresses_expiry", impact_suppresses_expiry);
    run("appearance_delay_contract", appearance_delay_contract);
    run("endpoint_and_retained_aim", endpoint_and_retained_aim);
    run("cubic_height_and_boundary", cubic_height_and_boundary);
    run("default_endpoints_and_lifetime", default_endpoints_and_lifetime);
    run("unsupported_and_encoding", unsupported_and_encoding);
    run("oversized_flight_allowance", oversized_flight_allowance);
    run("seeded_spatial_offsets", seeded_spatial_offsets);
    run("extension_and_construction_states", extension_and_construction_states);
    run("seeded_worker_and_target_independence", seeded_worker_and_target_independence);
    run("unit_salvo_launch_axis", unit_salvo_launch_axis);
    if (failures) return 1;
    std::cout << "rocket endpoint contracts passed (WAD-04, RFL-01..09)\n";
    return 0;
}
