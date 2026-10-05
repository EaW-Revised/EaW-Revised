#include "../../src/sim/tactical/combat_internal.hpp"
#include "../../src/sim/tactical/collection.hpp"
#include "../../src/sim/tactical/rocket_internal.hpp"

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
        if (!step) return;
        projectile = step.value().projectile;
    }
    projectile = launch(flight, at(300), q(100));
    for (std::uint32_t frame = 0; frame < 4; ++frame) {
        auto step = d::step_projectile(world, projectile, scratch);
        expect(step && step.value().expired == (frame == 3), "RFL-08: positive travel allowance takes precedence over lifetime");
        if (!step) return;
        projectile = step.value().projectile;
    }
    expect(static_cast<bool>(first), "RFL-08: fractional lifetime service succeeds");
}
void unsupported_and_encoding() {
    auto profile = rocket_profile();
    profile.curve_offset = q(1); expect(!d::valid_flight(profile), "RFL-03: custom curve remains explicitly unsupported");
    profile.curve_offset = {}; profile.target_radius = false;
    expect(!d::valid_flight(profile), "RFL-05: post-aim extension remains explicitly unsupported");
    auto projectile = launch(rocket_profile());
    std::vector<std::uint8_t> before, after;
    d::append_projectile(before, projectile);
    expect(static_cast<bool>(d::prepare_rocket_path(projectile)), "RFL-04: path prepares for hash test");
    d::append_projectile(after, projectile);
    expect(before != after, "RFL-01/04: initialized path is hashed projectile state");
    before = after; after.clear(); projectile.flight->aim.z = q(1); d::append_projectile(after, projectile);
    expect(before != after, "RFL-01: retained aim height participates in projectile hash");
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
} // namespace
int main() {
    appearance_delay_contract();
    endpoint_and_retained_aim(); cubic_height_and_boundary(); default_endpoints_and_lifetime(); unsupported_and_encoding();
    if (failures) return 1;
    std::cout << "rocket endpoint contracts passed (WAD-04, RFL-01..08)\n";
    return 0;
}
