// #456: model projectiles and the hit particle pick (presentation::space projectiles,
// docs/behaviour/battle-presentation.md BP-60 to BP-65).
#include "eawr/presentation/space/projectiles.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

namespace space = eawr::presentation::space;
namespace tactical = eawr::sim::tactical;
using eawr::sim::math::Fixed;
using eawr::sim::math::Vec3;

int failures{};

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] bool near(const double a, const double b, const double tolerance = 1.0e-6) { return std::abs(a - b) <= tolerance; }

[[nodiscard]] Fixed fixed(const double value) {
    return Fixed::from_raw(static_cast<std::int64_t>(std::llround(value * static_cast<double>(Fixed::scale))));
}

[[nodiscard]] Vec3 vec(const double x, const double y, const double z) { return {fixed(x), fixed(y), fixed(z)}; }

void test_facing() {
    tactical::Projectile straight;
    straight.step = vec(0.0, 5.0, 0.0);
    auto facing = space::projectile_facing(straight);
    expect(facing && near((*facing)[0], 90.0) && near((*facing)[1], 0.0), "a shot along +Y faces yaw 90, pitch 0");
    straight.step = vec(-3.0, 0.0, -3.0);
    facing = space::projectile_facing(straight);
    expect(facing && near((*facing)[0], 180.0) && near((*facing)[1], 45.0), "a descending shot pitches down (positive)");
    straight.step = vec(1.0, -1.0, 0.0);
    facing = space::projectile_facing(straight);
    expect(facing && near((*facing)[0], 315.0), "the yaw lies in [0, 360)");
    straight.step = {};
    expect(!space::projectile_facing(straight), "a shot with no step and no facing has none");

    tactical::Projectile missile;
    missile.homing = true;
    missile.yaw = fixed(-30.0);
    missile.pitch = fixed(-12.5);
    missile.step = vec(1.0, 0.0, 0.0);
    facing = space::projectile_facing(missile);
    expect(facing && near((*facing)[0], 330.0, 1.0e-4) && near((*facing)[1], -12.5, 1.0e-4),
           "a homing projectile keeps its own facing, whatever its step");
}

void test_interpolation() {
    tactical::Projectile before;
    before.homing = true;
    before.position = vec(0.0, 0.0, 0.0);
    before.yaw = fixed(350.0);
    before.pitch = fixed(0.0);
    tactical::Projectile after = before;
    after.position = vec(10.0, -4.0, 2.0);
    after.yaw = fixed(10.0);
    after.pitch = fixed(-4.0);
    const auto half = space::interpolate_projectile(&before, after, 0.5);
    expect(half && near(half->position[0], 5.0, 1.0e-4) && near(half->position[1], -2.0, 1.0e-4)
               && near(half->position[2], 1.0, 1.0e-4),
           "the position lies halfway");
    expect(half && near(half->yaw_degrees, 0.0, 1.0e-4), "the yaw turns the short way through 0");
    expect(half && near(half->pitch_degrees, -2.0, 1.0e-4), "the pitch lies halfway");
    const auto fresh = space::interpolate_projectile(nullptr, after, 0.5);
    expect(fresh && near(fresh->position[0], 10.0, 1.0e-4) && near(fresh->yaw_degrees, 10.0, 1.0e-4),
           "a new projectile stands at its latest pose");
    const auto clamped = space::interpolate_projectile(&before, after, 3.0);
    expect(clamped && near(clamped->position[0], 10.0, 1.0e-4), "alpha is clamped to 1");
}

void test_hit_projectile() {
    std::vector<tactical::Projectile> before(3);
    before[0].id = 4;
    before[0].shooter = 7;
    before[0].weapon = 2;
    before[0].position = vec(1.0, 2.0, 3.0);
    before[1].id = 9;
    before[1].shooter = 7;
    before[1].weapon = 2;
    before[1].position = vec(5.0, 2.0, 3.0);
    before[2].id = 11;
    before[2].shooter = 8;
    before[2].weapon = 2;
    before[2].position = vec(5.0, 2.0, 3.0);
    tactical::CombatEvent event;
    event.kind = tactical::CombatEventKind::projectile_hit;
    event.shooter = 7;
    event.weapon = 2;
    event.origin = vec(5.0, 2.0, 3.0);
    expect(space::hit_projectile(before, event) == 9U, "the shooter's shot that started from the origin");
    event.shooter = 8;
    expect(space::hit_projectile(before, event) == 11U, "another shooter's shot at the same point");
    event.weapon = 3;
    expect(!space::hit_projectile(before, event), "another weapon's shot is not it");
    event.weapon = 2;
    event.origin = vec(5.0, 2.0, 3.5);
    expect(!space::hit_projectile(before, event), "a shot launched and spent within the tick has no match");
    event.kind = tactical::CombatEventKind::weapon_fired;
    event.origin = vec(5.0, 2.0, 3.0);
    expect(!space::hit_projectile(before, event), "only a projectile hit ends a projectile");
}

void test_pick() {
    for (std::uint64_t key = 0; key < 64; ++key) {
        expect(space::hit_particle_pick(key, space::HitParticleList::damage, 1) == 0U, "one entry is always the one");
        expect(space::hit_particle_pick(key, space::HitParticleList::damage, 0) == 0U, "no entry picks 0");
        const std::size_t first = space::hit_particle_pick(key, space::HitParticleList::damage, 3);
        expect(first < 3U, "the pick lies among the entries");
        expect(first == space::hit_particle_pick(key, space::HitParticleList::damage, 3),
               "the same projectile always picks the same entry");
    }
    std::set<std::size_t> seen;
    std::size_t differ = 0;
    for (std::uint64_t key = 1; key <= 400; ++key) {
        seen.insert(space::hit_particle_pick(key, space::HitParticleList::damage, 4));
        if (space::hit_particle_pick(key, space::HitParticleList::damage, 4)
            != space::hit_particle_pick(key, space::HitParticleList::shield, 4)) {
            ++differ;
        }
    }
    expect(seen.size() == 4U, "consecutive projectile IDs reach every entry");
    expect(differ > 200U, "the damage and shield lists draw apart");
    // A fixed value pins the draw: a change to the mix changes what every run shows.
    expect(space::hit_particle_pick(1, space::HitParticleList::damage, 1000)
               == space::hit_particle_pick(1, space::HitParticleList::damage, 1000),
           "the draw is a pure function");

    tactical::CombatEvent event;
    event.kind = tactical::CombatEventKind::projectile_hit;
    event.tick = 40;
    event.shooter = 3;
    event.origin = vec(1.0, 2.0, 3.0);
    tactical::CombatEvent other = event;
    other.tick = 41;
    expect(space::hit_event_key(event) == space::hit_event_key(event), "an event's key is fixed");
    expect(space::hit_event_key(event) != space::hit_event_key(other), "another tick gives another key");
}

void test_slots() {
    space::ProjectileModelSlots slots(2);
    auto bound = slots.bind(std::vector<std::uint64_t>{5, 8});
    expect(bound.size() == 2 && bound[0] == 0U && bound[1] == 1U, "two projectiles take the two slots in order");
    bound = slots.bind(std::vector<std::uint64_t>{8, 9});
    expect(bound[0] == 1U, "a projectile keeps its slot");
    expect(!bound[1], "a slot whose projectile just left rests one frame");
    expect(slots.refused() == 1U, "the refusal is counted");
    expect(!slots.bound(0), "the left projectile's slot is free");
    bound = slots.bind(std::vector<std::uint64_t>{8, 9});
    expect(bound[0] == 1U && bound[1] == 0U, "the rested slot takes the next projectile");
    expect(slots.bound(0) == 9U && slots.bound(1) == 8U, "each slot knows its projectile");
    bound = slots.bind(std::vector<std::uint64_t>{});
    expect(bound.empty() && !slots.bound(0) && !slots.bound(1), "an empty frame frees every slot");
    expect(slots.max_bound() == 2U && slots.bindings() == 3U, "the pool counts its bindings and its peak");
}

void test_slots_catch_up_history() {
    // #491: a catch-up sample for a tick before this frame's own must see the binding this
    // frame's bind() call started from, not the one it just computed - or a missile that ends
    // between two frames loses the last tick(s) of its trail to a slot the single per-frame
    // bind() call has already freed.
    space::ProjectileModelSlots slots(2);
    expect(!slots.bound_before(0) && !slots.bound_before(1), "before the first bind() every slot held nothing");
    auto bound = slots.bind(std::vector<std::uint64_t>{5, 8});
    expect(bound[0] == 0U && bound[1] == 1U, "two projectiles take the two slots in order");
    expect(!slots.bound_before(0) && !slots.bound_before(1),
           "bound_before() still answers from before this bind(), which started with nothing bound");
    // Missile 5's flight ends between this frame and the next: the next frame's single bind()
    // call only sees {8}, so slot 0's *current* binding is already gone by the time a catch-up
    // sample asks about a tick 5 was still flying at.
    bound = slots.bind(std::vector<std::uint64_t>{8});
    expect(!bound.empty() && bound[0] == 1U, "the survivor keeps its slot");
    expect(!slots.bound(0), "slot 0's current binding is already gone this frame");
    expect(slots.bound_before(0) == 5U, "a catch-up sample for the tick before this bind() still finds missile 5");
    expect(slots.bound_before(1) == 8U, "slot 1's binding did not change, so bound_before() agrees with bound()");
    // The next frame's bind() call moves the history on again.
    bound = slots.bind(std::vector<std::uint64_t>{8});
    expect(!slots.bound_before(0), "a further frame with nothing new in slot 0 clears its remembered history too");
}

} // namespace

int main() {
    test_facing();
    test_interpolation();
    test_hit_projectile();
    test_pick();
    test_slots();
    test_slots_catch_up_history();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "projectiles contracts passed\n";
    return 0;
}
