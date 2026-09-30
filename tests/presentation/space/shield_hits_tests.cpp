// #415: how a live battle orients and places a shield hit (presentation::space shield_hits,
// docs/behaviour/battle-presentation.md BP-10, BP-17 to BP-19). Synthetic inputs only.
#include "eawr/presentation/space/shield_hits.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace space = eawr::presentation::space;
using space::Vec3d;

int failures{};

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] bool near(const double a, const double b, const double tolerance = 1.0e-9) {
    return std::abs(a - b) <= tolerance;
}
[[nodiscard]] bool near(const Vec3d& a, const Vec3d& b, const double tolerance = 1.0e-9) {
    return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) && near(a[2], b[2], tolerance);
}
[[nodiscard]] double dot(const Vec3d& a, const Vec3d& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
[[nodiscard]] Vec3d cross(const Vec3d& a, const Vec3d& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
[[nodiscard]] Vec3d unit(const Vec3d& a) {
    const double size = std::sqrt(dot(a, a));
    return {a[0] / size, a[1] / size, a[2] / size};
}
[[nodiscard]] std::string text(const Vec3d& a) {
    return "(" + std::to_string(a[0]) + ", " + std::to_string(a[1]) + ", " + std::to_string(a[2]) + ")";
}

void direction_rule() {
    // BP-17 without a SHIELD sub-object: back along the flight, normalised.
    expect(near(space::shield_hit_direction(std::nullopt, {25.0, 0.0, 0.0}), {-1.0, 0.0, 0.0}),
           "BP-17: no shield mesh faces back along the flight");
    expect(near(space::shield_hit_direction(std::nullopt, {3.0, -4.0, 0.0}), {-0.6, 0.8, 0.0}),
           "BP-17: the reversed flight is normalised");
    expect(near(space::shield_hit_direction(std::nullopt, {0.0, 0.0, 0.0}), {0.0, 0.0, 0.0}),
           "BP-17: a zero flight gives the zero direction");
    // With one: the mesh normal, turned round only when it points along the flight.
    expect(near(space::shield_hit_direction(Vec3d{0.0, 1.0, 0.0}, {0.0, -10.0, 0.0}), {0.0, 1.0, 0.0}),
           "BP-17: a normal against the flight is kept");
    expect(near(space::shield_hit_direction(Vec3d{0.0, -1.0, 0.0}, {0.0, -10.0, 0.0}), {0.0, 1.0, 0.0}),
           "BP-17: a normal along the flight is turned round");
    expect(near(space::shield_hit_direction(Vec3d{1.0, 0.0, 0.0}, {0.0, -10.0, 0.0}), {1.0, 0.0, 0.0}),
           "BP-17: a normal across the flight (zero dot) is kept");
    expect(near(space::shield_hit_direction(Vec3d{0.6, 0.8, 0.0}, {-3.0, 1.0, 0.0}), {0.6, 0.8, 0.0}),
           "BP-17: the normal is not the reversed flight");
}

void axes_rule() {
    // BP-18, a shot flying along +X: yaw 180, pitch 0 + 90.
    const space::ShieldHitAxes east = space::shield_hit_axes({-1.0, 0.0, 0.0}, true);
    expect(near(east.z, {-1.0, 0.0, 0.0}), "BP-18: local +Z lies along the direction, got " + text(east.z));
    expect(near(east.y, {0.0, -1.0, 0.0}), "BP-18: local +Y is Rz(yaw) +Y, got " + text(east.y));
    expect(near(east.x, {0.0, 0.0, -1.0}), "BP-18: local +X points down for a level shot, got " + text(east.x));
    // The retired reading (#415): turning the yaw by 90 left the particle's +Z vertical for every
    // level shot. The 90 degrees belong to the pitch.
    expect(!near(std::abs(east.z[2]), 1.0, 1.0e-3), "BP-18: a level shot's particle +Z is not vertical");

    // Any direction: +Z along it, +Y horizontal, a right-handed orthonormal frame.
    const std::vector<Vec3d> directions{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, -1.0, 0.0}, unit({1.0, 1.0, 1.0}),
                                        unit({-2.0, 0.5, -1.0}), unit({0.3, -0.9, 0.2}), unit({-0.1, -0.1, 0.99})};
    for (const Vec3d& direction : directions) {
        const space::ShieldHitAxes axes = space::shield_hit_axes(direction, true);
        const std::string where = " for " + text(direction);
        expect(near(axes.z, direction, 1.0e-12), "BP-18: +Z along the direction" + where + ", got " + text(axes.z));
        expect(near(axes.y[2], 0.0), "BP-18: +Y horizontal" + where);
        expect(near(dot(axes.x, axes.x), 1.0) && near(dot(axes.y, axes.y), 1.0) && near(dot(axes.z, axes.z), 1.0),
               "BP-18: unit axes" + where);
        expect(near(dot(axes.x, axes.y), 0.0) && near(dot(axes.y, axes.z), 0.0) && near(dot(axes.x, axes.z), 0.0),
               "BP-18: orthogonal axes" + where);
        expect(near(cross(axes.x, axes.y), axes.z), "BP-18: right-handed" + where);
    }
    // Straight up or down: the debug build's facing gives yaw 0 when x and y are both zero.
    const space::ShieldHitAxes up = space::shield_hit_axes({0.0, 0.0, 1.0}, true);
    expect(near(up.z, {0.0, 0.0, 1.0}) && near(up.y, {0.0, 1.0, 0.0}), "BP-18: straight up keeps yaw 0");
    const space::ShieldHitAxes down = space::shield_hit_axes({0.0, 0.0, -1.0}, true);
    expect(near(down.z, {0.0, 0.0, -1.0}) && near(down.y, {0.0, 1.0, 0.0}), "BP-18: straight down keeps yaw 0");
    // The zero direction still has a facing: yaw 0, pitch 90.
    const space::ShieldHitAxes zero = space::shield_hit_axes({0.0, 0.0, 0.0}, true);
    expect(near(zero.z, {1.0, 0.0, 0.0}) && near(zero.y, {0.0, 1.0, 0.0}), "BP-18: the zero direction faces +X");

    // A free particle (not attached to the collision) adds the model's quarter turn about its own
    // +Z: +Z stays on the direction, +X goes to the facing's +Y and +Y to its -X.
    for (const Vec3d& direction : directions) {
        const space::ShieldHitAxes attached = space::shield_hit_axes(direction, true);
        const space::ShieldHitAxes free = space::shield_hit_axes(direction, false);
        const std::string where = " for " + text(direction);
        expect(near(free.z, attached.z), "BP-18: the quarter turn keeps +Z" + where);
        expect(near(free.x, attached.y), "BP-18: the quarter turn sends +X to the facing's +Y" + where);
        expect(near(free.y, {-attached.x[0], -attached.x[1], -attached.x[2]}),
               "BP-18: the quarter turn sends +Y to the facing's -X" + where);
    }
}

void segment_rule() {
    // BP-19: a unit square in the plane x = 10 (two triangles, opposite windings) and one at x = 5.
    const std::vector<space::ShieldTriangle> far{{{10.0, -1.0, -1.0}, {10.0, 1.0, -1.0}, {10.0, 1.0, 1.0}},
                                                 {{10.0, -1.0, -1.0}, {10.0, -1.0, 1.0}, {10.0, 1.0, 1.0}}};
    auto hit = space::first_shield_hit(far, {0.0, 0.2, -0.3}, {20.0, 0.0, 0.0});
    expect(hit && near(hit->fraction, 0.5) && near(hit->contact, {10.0, 0.2, -0.3}),
           "BP-19: the segment meets the plane half way");
    expect(hit && near(std::abs(hit->normal[0]), 1.0), "BP-19: the face normal is the plane's");
    // Two-sided: the same square from the other side.
    hit = space::first_shield_hit(far, {20.0, 0.2, 0.3}, {-20.0, 0.0, 0.0});
    expect(hit && near(hit->contact, {10.0, 0.2, 0.3}), "BP-19: the test is two-sided");
    // Missed, short of it, behind the start.
    expect(!space::first_shield_hit(far, {0.0, 1.5, 0.0}, {20.0, 0.0, 0.0}), "BP-19: outside the triangles misses");
    expect(!space::first_shield_hit(far, {0.0, 0.0, 0.0}, {9.0, 0.0, 0.0}), "BP-19: a segment ending short misses");
    expect(!space::first_shield_hit(far, {12.0, 0.0, 0.0}, {5.0, 0.0, 0.0}), "BP-19: a surface behind the start misses");
    // The nearest wins, whatever the triangle order.
    std::vector<space::ShieldTriangle> both = far;
    both.push_back({{5.0, -1.0, -1.0}, {5.0, 1.0, -1.0}, {5.0, 1.0, 1.0}});
    both.push_back({{5.0, -1.0, -1.0}, {5.0, -1.0, 1.0}, {5.0, 1.0, 1.0}});
    hit = space::first_shield_hit(both, {0.0, 0.1, 0.1}, {20.0, 0.0, 0.0});
    expect(hit && near(hit->contact[0], 5.0), "BP-19: the nearest surface wins");
    // The normal is the wound face normal, which BP-17 then turns against the flight.
    const std::vector<space::ShieldTriangle> wound{{{10.0, 0.0, 0.0}, {10.0, 1.0, 0.0}, {10.0, 0.0, 1.0}}};
    hit = space::first_shield_hit(wound, {0.0, 0.2, 0.2}, {20.0, 0.0, 0.0});
    expect(hit && near(hit->normal, {1.0, 0.0, 0.0}), "BP-19: the normal follows the winding");
    expect(hit && near(space::shield_hit_direction(hit->normal, {20.0, 0.0, 0.0}), {-1.0, 0.0, 0.0}),
           "BP-17: the wound normal is turned to face the shooter");
}

void segment_ends() {
    // The fraction lies in (0, 1): FoC rejects a hit at the segment's end (its initial result
    // fraction) as well as one at its start. The plane x = 1, with a determinant of -4 so the fractions are exact.
    const std::vector<space::ShieldTriangle> plane{{{1.0, -1.0, -1.0}, {1.0, 1.0, -1.0}, {1.0, -1.0, 1.0}}};
    expect(!space::first_shield_hit(plane, {0.0, -0.5, -0.5}, {1.0, 0.0, 0.0}),
           "BP-19: a surface exactly at the segment's end (fraction 1) is not met");
    expect(!space::first_shield_hit(plane, {1.0, -0.5, -0.5}, {1.0, 0.0, 0.0}),
           "BP-19: a surface exactly at the segment's start (fraction 0) is not met");
    const auto inside = space::first_shield_hit(plane, {0.0, -0.5, -0.5}, {1.0 + 1.0e-9, 0.0, 0.0});
    expect(inside && inside->fraction > 0.0 && inside->fraction < 1.0,
           "BP-19: a surface just short of the segment's end is met, strictly inside (0, 1)");
    const auto start = space::first_shield_hit(plane, {1.0 - 1.0e-9, -0.5, -0.5}, {1.0, 0.0, 0.0});
    expect(start && start->fraction > 0.0 && start->fraction < 1.0,
           "BP-19: a surface just past the segment's start is met, strictly inside (0, 1)");
}

// A closed box from -half to +half on every model axis, twelve triangles.
[[nodiscard]] std::vector<space::ShieldTriangle> box_mesh(const double half) {
    std::vector<space::ShieldTriangle> result;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const std::size_t u = (axis + 1) % 3;
        const std::size_t v = (axis + 2) % 3;
        for (const double side : {-half, half}) {
            const auto corner = [&](const double a, const double b) {
                Vec3d point{};
                point[axis] = side;
                point[u] = a;
                point[v] = b;
                return point;
            };
            result.push_back({corner(-half, -half), corner(half, -half), corner(half, half)});
            result.push_back({corner(-half, -half), corner(half, half), corner(-half, half)});
        }
    }
    return result;
}

void flight_rule() {
    // BP-19 as the viewer casts it: from the projectile's frame-step start, never from behind it.
    // A bubble whose surface the flight line meets at world x = -1 and x = +1.
    const space::ShieldCollisionMesh bubble = space::make_shield_collision_mesh(box_mesh(1.0));
    const space::LivePose pose{};
    // From outside: the near side, within a step long enough to reach it.
    auto hit = space::shield_flight_hit(bubble, pose, {-5.0, 0.1, 0.2}, {3.0, 0.0, 0.0}, 10.0);
    expect(hit && near(hit->hit.contact, {-1.0, 0.1, 0.2}) && hit->in_step,
           "BP-19: a step from outside meets the near side in the step");
    expect(hit && near(space::shield_hit_direction(hit->hit.normal, {1.0, 0.0, 0.0}), {-1.0, 0.0, 0.0}),
           "BP-17: the near side faces the shooter");
    // The step starts inside the bubble: FoC's step meets the far side, where the shot leaves it.
    hit = space::shield_flight_hit(bubble, pose, {0.0, 0.1, 0.2}, {3.0, 0.0, 0.0}, 2.0);
    expect(hit && near(hit->hit.contact, {1.0, 0.1, 0.2}, 1.0e-9) && hit->in_step,
           "BP-19: a step starting inside the bubble meets its exit, not the entry behind the start");
    // Its step stops short of the far side (the collision box took the shot first): the cast goes
    // on from the same start and still never looks behind it.
    hit = space::shield_flight_hit(bubble, pose, {0.0, 0.1, 0.2}, {3.0, 0.0, 0.0}, 0.5);
    expect(hit && near(hit->hit.contact, {1.0, 0.1, 0.2}, 1.0e-9) && !hit->in_step,
           "BP-19: a short step inside the bubble goes on to the exit, not back to the entry");
    // A step from outside that stops short also goes on to the near side.
    hit = space::shield_flight_hit(bubble, pose, {-5.0, 0.1, 0.2}, {1.0, 0.0, 0.0}, 2.0);
    expect(hit && near(hit->hit.contact, {-1.0, 0.1, 0.2}, 1.0e-9) && !hit->in_step,
           "BP-19: a step short of the bubble goes on to its near side");
    // Past the bubble, flying away: nothing ahead, so no mesh hit (the box contact stays).
    expect(!space::shield_flight_hit(bubble, pose, {3.0, 0.1, 0.2}, {1.0, 0.0, 0.0}, 2.0),
           "BP-19: a shot past the bubble does not look behind its start");
    // Posed: the contact and the normal come back in world space.
    const space::LivePose moved{{100.0, -20.0, 5.0}, 90.0, 0.0, 2.0};
    hit = space::shield_flight_hit(bubble, moved, {100.0, -30.0, 5.5}, {0.0, 1.0, 0.0}, 20.0);
    expect(hit && near(hit->hit.contact, {100.0, -22.0, 5.5}, 1.0e-9), "BP-19: the posed bubble's near side");
    expect(hit && near(std::abs(hit->hit.normal[1]), 1.0, 1.0e-12), "BP-19: the posed normal is world space");
}

// A sphere of `bands` x 2 `bands` quads, two triangles each, in latitude order.
[[nodiscard]] std::vector<space::ShieldTriangle> sphere_mesh(const double radius, const int bands) {
    constexpr double pi = 3.14159265358979323846;
    const auto point = [&](const int band, const int step) {
        const double theta = pi * band / bands;
        const double phi = 2.0 * pi * step / (2 * bands);
        return Vec3d{radius * std::sin(theta) * std::cos(phi), radius * std::sin(theta) * std::sin(phi),
                     radius * std::cos(theta)};
    };
    std::vector<space::ShieldTriangle> result;
    for (int band = 0; band < bands; ++band) {
        for (int step = 0; step < 2 * bands; ++step) {
            result.push_back({point(band, step), point(band + 1, step), point(band + 1, step + 1)});
            result.push_back({point(band, step), point(band + 1, step + 1), point(band, step + 1)});
        }
    }
    return result;
}

void many_hits() {
    // Many hits on a dense mesh: the model-space cast gives the brute-force world-space answer
    // (every triangle posed, every triangle tested) while testing only a small share.
    const std::vector<space::ShieldTriangle> model = sphere_mesh(10.0, 48);
    const space::ShieldCollisionMesh mesh = space::make_shield_collision_mesh(model);
    const space::LivePose pose{{100.0, -50.0, 20.0}, 37.0, 12.0, 1.7};
    std::vector<space::ShieldTriangle> posed;
    const auto place = [&](const Vec3d& at) {
        return space::live_model_point(at, pose.position, pose.yaw_degrees, pose.roll_degrees, pose.scale);
    };
    for (const space::ShieldTriangle& triangle : model) posed.push_back({place(triangle.a), place(triangle.b), place(triangle.c)});
    std::uint64_t state = 0x2545F4914F6CDD1DULL;
    const auto random = [&state](const double low, const double high) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return low + (high - low) * static_cast<double>(state >> 11) / 9007199254740992.0;
    };
    space::ShieldCastStats stats;
    constexpr int casts = 2000;
    int met = 0;
    int agree = 0;
    for (int index = 0; index < casts; ++index) {
        const Vec3d start{random(40.0, 160.0), random(-110.0, 10.0), random(-40.0, 80.0)};
        const Vec3d aim{pose.position[0] + random(-20.0, 20.0), pose.position[1] + random(-20.0, 20.0),
                        pose.position[2] + random(-20.0, 20.0)};
        const Vec3d delta{aim[0] - start[0], aim[1] - start[1], aim[2] - start[2]};
        const auto fast = space::first_shield_hit(mesh, pose, start, delta, &stats);
        const auto slow = space::first_shield_hit(posed, start, delta);
        if (fast.has_value() != slow.has_value()) continue;
        if (!fast) {
            ++agree;
            continue;
        }
        ++met;
        if (near(fast->contact, slow->contact, 1.0e-6) && near(fast->normal, slow->normal, 1.0e-6)) ++agree;
    }
    expect(agree == casts, "many hits: the model-space cast agrees with the posed brute force (" + std::to_string(agree)
                               + " of " + std::to_string(casts) + ")");
    expect(met > casts / 4 && met < casts, "many hits: the casts both meet and miss the sphere");
    expect(stats.casts == casts, "many hits: every cast is counted");
    const std::uint64_t brute = static_cast<std::uint64_t>(casts) * model.size();
    expect(stats.triangles_tested * 5 < brute, "many hits: the broad phase skips most triangles (tested "
                                                   + std::to_string(stats.triangles_tested) + " of "
                                                   + std::to_string(brute) + ")");
    expect(stats.max_triangles_per_cast < model.size() / 2, "many hits: no cast tests half the mesh");
    // A cast far from the mesh is rejected by its sphere without a triangle test.
    const std::uint64_t before = stats.triangles_tested;
    expect(!space::first_shield_hit(mesh, pose, {0.0, 0.0, 0.0}, {0.0, 0.0, 10.0}, &stats)
               && stats.sphere_rejects > 0 && stats.triangles_tested == before,
           "many hits: a far cast stops at the mesh's sphere");
}

void placement_rule() {
    // R-ROT-01/R-ROT-04: a model's nose on model -Y heads along the yaw; the scale and position follow.
    expect(near(space::live_model_point({0.0, -1.0, 0.0}, {0.0, 0.0, 0.0}, 0.0, 0.0, 1.0), {1.0, 0.0, 0.0}),
           "placement: model -Y is the heading at yaw 0");
    expect(near(space::live_model_point({0.0, -2.0, 0.0}, {5.0, 6.0, 7.0}, 90.0, 0.0, 1.5), {5.0, 9.0, 7.0}),
           "placement: yaw 90, scale 1.5 and the position");
    // Roll about the forward axis: the quarter turn puts model -X on -Y, and a roll of 90 takes -Y to -Z.
    const Vec3d side = space::live_model_point({-1.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, 0.0, 90.0, 1.0);
    expect(near(side, {0.0, 0.0, -1.0}), "placement: roll 90 turns the model's -X from -Y to -Z, got " + text(side));
}

} // namespace

int main() {
    direction_rule();
    axes_rule();
    segment_rule();
    segment_ends();
    flight_rule();
    many_hits();
    placement_rule();
    if (failures != 0) {
        std::cerr << failures << " shield hit contract(s) failed\n";
        return 1;
    }
    std::cout << "shield hit contracts passed\n";
    return 0;
}
