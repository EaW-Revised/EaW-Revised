#include "sun_retail_test_support.hpp"

namespace eawr_sun_retail_test {

// -- identity and helpers ----------------------------------------------------------

void test_identity() {
    expect(space::sun_retail_policy_id == "eawr-sun-mode7-retail-v1", "the retail policy id is pinned");
    expect(space::sun_retail_policy_id != space::sun_reference_policy_id, "the retail policy is a new identity");
    expect(space::sun_reference_policy_id == "eawr-sun-mode7-reference-av01-v1", "the reference id is unchanged");
    expect(space::sun_retail_min_distance == 1.0e-6 && space::sun_retail_unit_tolerance == 1.0e-4
               && space::sun_retail_min_up_sine == 1.0e-4,
           "the retail thresholds are pinned");

    // R-M7-01: low four bits at draw time; zeroing only for exactly 6 or 7.
    static_assert(space::sun_retail_draw_mode(7) == 7 && space::sun_retail_draw_mode(23) == 7
                  && space::sun_retail_draw_mode(39) == 7 && space::sun_retail_draw_mode(16) == 0
                  && space::sun_retail_draw_mode(6) == 6 && space::sun_retail_draw_mode(0xFFFFFFFFU) == 15);
    static_assert(space::sun_retail_rest_translation_zeroed(6) && space::sun_retail_rest_translation_zeroed(7)
                  && !space::sun_retail_rest_translation_zeroed(23) && !space::sun_retail_rest_translation_zeroed(22)
                  && !space::sun_retail_rest_translation_zeroed(0) && !space::sun_retail_rest_translation_zeroed(5)
                  && !space::sun_retail_rest_translation_zeroed(8));
    expect(space::sun_retail_draw_mode(0x107U) == 7 && !space::sun_retail_rest_translation_zeroed(0x107U),
           "a raw 0x107 draws as 7 and keeps its rest translation");

    std::set<std::string_view> names;
    for (std::uint8_t value = 0; value <= static_cast<std::uint8_t>(SunRetailStatus::sun_along_camera_up); ++value) {
        const auto name = space::to_string(static_cast<SunRetailStatus>(value));
        expect(name != "invalid" && !name.empty(), "status " + std::to_string(value) + " has a name");
        names.insert(name);
    }
    expect(names.size() == 14, "the 14 retail statuses have distinct names");
    expect(names.count("sun_direction_vertical") == 0, "the retail policy has no vertical-L status (B-07 rejected)");
}

// -- RT-01..RT-09 --------------------------------------------------------------------

void test_retail_oracles() {
    // RT-01 centred sun.
    {
        const RetailCase sun;
        const auto placement = sun.place();
        expect(placement.status == SunRetailStatus::placed && placement.detail.empty(), "RT-01 is placed");
        expect(std::abs(placement.distance - 1000.0) <= 1.0e-9 && placement.scale == 1.0, "RT-01 d = 1000, s = 1");
        expect_near3(placement.right, {1, 0, 0}, "RT-01 R");
        expect_near3(placement.view_up, {0, 0, 1}, "RT-01 U");
        expect_near3(placement.backward, {0, -1, 0}, "RT-01 B");
        expect_near3(placement.origin, {0, 990, 0}, "RT-01 origin E + d*L");
        expect_frame(placement, {1, 0, 0}, {0, -1, 0}, {0, 0, 1}, "RT-01");
        expect_vertex(sun, {1, 0, 1}, {1, 990, 1}, "RT-01 (1, 0, 1)");
        expect_vertex(sun, {1, 0, -1}, {1, 990, -1}, "RT-01 (1, 0, -1)");
        expect_vertex(sun, {0, 1, 0}, {0, 989, 0}, "RT-01 local +Y toward the eye");
        expect_vertex(sun, {0, -1, 0}, {0, 991, 0}, "RT-01 local -Y away from the eye");
        const auto b02 = reference_vertex(sun, {1000, 0, 0}, {1, 0, 1});
        expect(b02 && near3(*b02, {1, 1000, 1}), "RT-01 the unchanged reference still gives B-02's (1, 1000, 1)");
    }

    // RT-02 camera translation: the offset from the eye is RT-01's.
    {
        RetailCase sun;
        const auto first = sun.place();
        sun.eye = {500, -300, 40};
        sun.target = {500, -290, 40};
        const auto moved = sun.place();
        expect_near3(moved.origin, {500, 700, 40}, "RT-02 origin");
        expect_vertex(sun, {1, 0, 1}, {501, 700, 41}, "RT-02 (1, 0, 1)");
        expect_near3(minus(moved.origin, widen(sun.eye)), minus(first.origin, {0, -10, 0}),
                     "RT-02 origin - eye is RT-01's", 1.0e-9);
        const auto b02 = reference_vertex(sun, {1000, 0, 0}, {1, 0, 1});
        expect(b02 && near3(*b02, {1, 1000, 1}), "RT-02 B-02 stays at (1, 1000, 1) for the moved camera");
    }

    // RT-03 off-centre sun faces the eye, not the view direction.
    {
        RetailCase sun;
        sun.eye = {0, 0, 0};
        sun.target = {0, 1, 0};
        sun.toward_light = {0.70710677F, 0.70710677F, 0};
        const auto placement = sun.place();
        expect_near3(placement.origin, {707.10678, 707.10678, 0}, "RT-03 origin");
        expect_frame(placement, {0.70711, -0.70711, 0}, {-0.70711, -0.70711, 0}, {0, 0, 1}, "RT-03");
        expect_vertex(sun, {1, 0, 1}, {707.81388, 706.39966, 1}, "RT-03 (1, 0, 1)");
        expect_vertex(sun, {0, 1, 0}, {706.39966, 706.39966, 0}, "RT-03 (0, 1, 0)");
        const auto vertex = space::sun_retail_vertex(placement, {1, 0, 1});
        expect(vertex && !near3(*vertex, {708.10678, 707.10678, 1}), "RT-03 is not the screen-aligned quad");
        expect(!near3(placement.axes[1], placement.backward, 0.1), "RT-03 n is not B");
    }

    // RT-04 parent chain and own-translation direction are ignored.
    {
        const RetailCase plain;
        const auto base = plain.place();
        const Rigid12 parent{0, -1, 0, 0, 1, 0, 0, 50, 0, 0, 1, 0};
        const Rigid12 child{1, 0, 0, 0, 0, 0.8660254F, -0.5F, 600, 0, 0.5F, 0.8660254F, 800};
        RetailCase sun;
        sun.bones = {record("Parent", -1, 0, parent), record("Sun", 0, 7, child)};
        const auto check = [&base](const RetailCase& variant, const std::string& label) {
            const auto placement = variant.place();
            expect(placement.status == SunRetailStatus::placed && std::abs(placement.distance - 1000.0) <= 1.0e-9
                       && near3(placement.origin, base.origin, 1.0e-9) && near3(placement.axes[0], base.axes[0], 1.0e-12)
                       && near3(placement.axes[1], base.axes[1], 1.0e-12)
                       && near3(placement.axes[2], base.axes[2], 1.0e-12),
                   label + " is identical to RT-01");
        };
        check(sun, "RT-04 rotated parent, rotated child, own translation (0, 600, 800)");
        expect_vertex(sun, {1, 0, 1}, {1, 990, 1}, "RT-04 (1, 0, 1)");
        for (const float x : {1000.0F, -1000.0F}) {
            RetailCase direction = sun;
            direction.bones[1].relative_transform[3] = x;
            direction.bones[1].relative_transform[7] = 0;
            direction.bones[1].relative_transform[11] = 0;
            check(direction, "RT-04 own translation (" + std::to_string(x) + ", 0, 0)");
        }
        RetailCase moved_parent = sun;
        moved_parent.bones[0].relative_transform = translation(-7777, 12345, 999);
        check(moved_parent, "RT-04 a translated, unrotated parent");
        RetailCase turned_parent = sun;
        turned_parent.bones[0].relative_transform = {0.8660254F, 0, 0.5F, 3, 0, 1, 0, -4, -0.5F, 0, 0.8660254F, 5};
        check(turned_parent, "RT-04 a parent turned about Y");
        RetailCase lone = plain;
        lone.bones.erase(lone.bones.begin());
        lone.bones[0].parent = -1;
        lone.mesh_bone = 0;
        check(lone, "RT-04 a parentless mode-7 bone");
    }

    // RT-05 a vertical unit L is placed under a tilted camera.
    {
        RetailCase sun;
        sun.eye = {0, 0, 0};
        sun.target = {1, 0, 1};
        sun.toward_light = {0, 0, 1};
        const auto placement = sun.place();
        expect_near3(placement.right, {0, -1, 0}, "RT-05 R");
        expect_near3(placement.view_up, {-0.70711, 0, 0.70711}, "RT-05 U");
        expect_near3(placement.backward, {-0.70711, 0, -0.70711}, "RT-05 B");
        expect_near3(placement.origin, {0, 0, 1000}, "RT-05 origin");
        expect_frame(placement, {0, -1, 0}, {0, 0, -1}, {-1, 0, 0}, "RT-05");
        expect_vertex(sun, {1, 0, 1}, {-1, -1, 1000}, "RT-05 (1, 0, 1)");
        expect_vertex(sun, {0, 1, 0}, {0, 0, 999}, "RT-05 (0, 1, 0)");
        space::SunReferenceInput reference;
        reference.bones = sun.bones;
        reference.mesh_bone = 1;
        reference.eye = sun.eye;
        reference.target = sun.target;
        reference.toward_sun = sun.toward_light;
        expect(space::sun_reference_placement(reference).status == space::SunPlacementStatus::sun_direction_vertical,
               "RT-05 the reference evaluator still rejects it as sun_direction_vertical");
    }

    // RT-06 degeneracies are rejected; the boundary case is placed.
    {
        RetailCase along;
        along.toward_light = {0, 0, 1};
        expect_status(along, SunRetailStatus::sun_along_camera_up, "RT-06a L = +Z = U");
        along.toward_light = {0, 0, -1};
        expect_status(along, SunRetailStatus::sun_along_camera_up, "RT-06a L = -Z = -U");
        expect_status(RetailCase({0, 0, 0}), SunRetailStatus::sun_distance_zero, "RT-06b d = 0");
        RetailCase zero;
        zero.toward_light = {0, 0, 0};
        expect_status(zero, SunRetailStatus::sun_direction_zero, "RT-06c L = 0");
        RetailCase boundary;
        boundary.toward_light = {0, 0.0009999995F, 0.9999995F};
        const auto placed = boundary.place();
        expect_near3(placed.origin, {0, -9.0, 999.99952}, "RT-06d |U x n| = 1e-3 origin");
        expect_vertex(boundary, {1, 0, 1}, {1, -10.0, 1000.00052}, "RT-06d (1, 0, 1)");
        for (const float s : {2.0e-4F, 1.1e-4F}) {
            boundary.toward_light = {0, s, 1};
            expect_status(boundary, SunRetailStatus::placed, "RT-06d |U x n| = " + std::to_string(s) + " is placed");
        }
        for (const float s : {1.0e-5F, 5.0e-5F, 9.0e-5F}) {
            boundary.toward_light = {0, s, 1};
            expect_status(boundary, SunRetailStatus::sun_along_camera_up,
                          "RT-06d |U x n| = " + std::to_string(s) + " is rejected");
        }
    }

    // RT-07 general case: rotated parent, rotated child, oblique camera and L.
    {
        RetailCase sun;
        sun.bones = {record("Parent", -1, 0, {0, -1, 0, 20, 1, 0, 0, -40, 0, 0, 1, 5}),
                     record("Sun", 0, 7, {1, 0, 0, -300, 0, 0.8660254F, -0.5F, -100, 0, 0.5F, 0.8660254F, 50})};
        sun.eye = {10, 20, 5};
        sun.target = {-30, 70, -15};
        sun.toward_light = {-0.51214755F, 0.76822126F, 0.38411063F};
        const auto placement = sun.place();
        expect(std::abs(placement.distance - 320.15621) <= 1.0e-5, "RT-07 d = |(-300, -100, 50)|");
        expect_near3(placement.right, {0.78087, 0.62470, 0}, "RT-07 R");
        expect_near3(placement.view_up, {-0.18625, 0.23281, 0.95452}, "RT-07 U");
        expect_near3(placement.backward, {0.59628, -0.74536, 0.29814}, "RT-07 B");
        expect_near3(placement.origin, {-153.96722, 265.95081, 127.97540}, "RT-07 origin");
        expect_frame(placement, {0.83875, 0.54363, 0.03106}, {0.51215, -0.76822, -0.38411},
                     {0.18495, -0.33808, 0.92276}, "RT-07");
        expect_vertex(sun, {2, -3, 4}, {-153.08636, 267.99041, 132.88092}, "RT-07 (2, -3, 4)");
    }

    // RT-09 (documented only): a non-unit L is rejected, not placed at (0, 1990, 0).
    {
        RetailCase sun;
        sun.toward_light = {0, 2, 0};
        expect_status(sun, SunRetailStatus::sun_direction_not_unit, "RT-09 L = (0, 2, 0)");
    }
}

// -- RT-08 winding and frame invariants over deterministic samples --------------------

struct Lcg final {
    std::uint64_t state{0x5EED5EED0F16ULL};
    double next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 11) * (1.0 / 9007199254740992.0);
    }
    double range(const double low, const double high) { return low + (high - low) * next(); }
    SunVec3 direction() {
        for (;;) {
            const SunVec3 value{range(-1, 1), range(-1, 1), range(-1, 1)};
            const double length = norm3(value);
            if (length > 0.1 && length <= 1.0) return {value[0] / length, value[1] / length, value[2] / length};
        }
    }
};

Vec3f narrow(const SunVec3& value) {
    return {static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2])};
}

// Signed screen area (y up) of three points under the look-at camera, or
// empty when a point is not in front of the eye.
std::optional<double> screen_area(const SunVec3& eye, const SunVec3& forward, const SunVec3& right, const SunVec3& up,
                                  const std::array<SunVec3, 3>& points) {
    std::array<std::array<double, 2>, 3> screen{};
    for (std::size_t index = 0; index < 3; ++index) {
        const SunVec3 relative = minus(points[index], eye);
        const double depth = dot3(relative, forward);
        if (!(depth > 0.0)) return std::nullopt;
        screen[index] = {dot3(relative, right) / depth, dot3(relative, up) / depth};
    }
    return (screen[1][0] - screen[0][0]) * (screen[2][1] - screen[0][1])
         - (screen[1][1] - screen[0][1]) * (screen[2][0] - screen[0][0]);
}

void test_winding_and_frame_samples() {
    Lcg random;
    constexpr int samples = 20000;
    int placed = 0, in_front = 0, counter_clockwise = 0, b02_counter_clockwise = 0, frame_ok = 0;
    for (int sample = 0; sample < samples; ++sample) {
        RetailCase sun;
        const SunVec3 eye{random.range(-5000, 5000), random.range(-5000, 5000), random.range(-5000, 5000)};
        const SunVec3 view = random.direction();
        sun.eye = narrow(eye);
        sun.target = narrow({eye[0] + 10 * view[0], eye[1] + 10 * view[1], eye[2] + 10 * view[2]});
        sun.up = random.next() < 0.5 ? Vec3f{0, 0, 1} : narrow(random.direction());
        sun.toward_light = narrow(random.direction());
        const double d = random.range(100, 10000);
        const SunVec3 own = random.direction();
        sun.bones[1].relative_transform = translation(static_cast<float>(d * own[0]), static_cast<float>(d * own[1]),
                                                      static_cast<float>(d * own[2]));
        const auto placement = sun.place();
        if (placement.status != SunRetailStatus::placed) continue;
        ++placed;

        // An independent camera frame and quad frame from the inputs.
        const SunVec3 e = widen(sun.eye);
        const SunVec3 f = unit3(minus(widen(sun.target), e));
        const SunVec3 r = unit3(cross3(f, unit3(widen(sun.up))));
        const SunVec3 u = cross3(r, f);
        const SunVec3 l = widen(sun.toward_light);
        const SunVec3 n = unit3({-l[0], -l[1], -l[2]});
        // Roll from the camera up axis: Z is U projected onto the quad plane.
        const double un = dot3(u, n);
        const SunVec3 z = unit3({u[0] - un * n[0], u[1] - un * n[1], u[2] - un * n[2]});
        const SunVec3 x = cross3(z, n);
        const SunVec3 toward_eye = unit3(minus(e, placement.origin));
        const bool frame = near3(placement.axes[0], x, 1.0e-9) && near3(placement.axes[1], n, 1.0e-9)
            && near3(placement.axes[2], z, 1.0e-9) && near3(placement.axes[1], toward_eye, 1.0e-9)
            && std::abs(dot3(placement.axes[0], cross3(placement.axes[1], placement.axes[2])) + 1.0) <= 1.0e-9
            && std::abs(dot3(placement.axes[0], u)) <= 1.0e-9
            && std::abs(norm3(minus(placement.origin, e)) - placement.distance * norm3(l)) <= 1.0e-6 * placement.distance;
        if (frame) ++frame_ok;

        // RT-08: sun in front (L . forward >= 0.2) and away from U.
        if (dot3(l, f) < 0.2 || norm3(cross3(u, n)) < 1.0e-2) continue;
        std::array<SunVec3, 3> retail{};
        std::array<SunVec3, 3> b02{};
        const std::array<Vec3f, 3> triangle{{{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}}};
        for (std::size_t index = 0; index < 3; ++index) {
            retail[index] = space::sun_retail_vertex(placement, triangle[index]).value_or(SunVec3{});
            const Vec3f& p = triangle[index];
            b02[index] = {placement.origin[0] + p.x * r[0] + p.z * u[0], placement.origin[1] + p.x * r[1] + p.z * u[1],
                          placement.origin[2] + p.x * r[2] + p.z * u[2]};
        }
        const auto area = screen_area(e, f, r, u, retail);
        const auto b02_area = screen_area(e, f, r, u, b02);
        if (!area || !b02_area) continue;
        ++in_front;
        if (*area > 0.0) ++counter_clockwise;
        if (*b02_area > 0.0) ++b02_counter_clockwise;
    }
    expect(placed > samples * 9 / 10, "most random samples are placed (" + std::to_string(placed) + ")");
    expect(frame_ok == placed, "frame invariants hold for every placed sample: n toward the eye, det -1, X . U = 0, "
                               "Z = U projected onto the quad plane, |origin - E| = d|L| ("
                                   + std::to_string(frame_ok) + "/" + std::to_string(placed) + ")");
    expect(in_front > 5000, "enough RT-08 samples have the sun in front (" + std::to_string(in_front) + ")");
    expect(counter_clockwise == in_front, "RT-08 the XZ triangle projects counter-clockwise ("
                                              + std::to_string(counter_clockwise) + "/" + std::to_string(in_front) + ")");
    expect(b02_counter_clockwise == in_front, "RT-08 the same winding as B-02 at the same origin");
    std::cout << "RT-08: " << counter_clockwise << "/" << in_front << " in-front samples counter-clockwise; "
              << frame_ok << "/" << placed << " placed samples satisfy the frame invariants\n";
}

// -- camera translation, rotation and roll ----------------------------------------------

void test_camera_translation() {
    const RetailCase base = general_case();
    const auto first = base.place();
    const auto first_vertex = space::sun_retail_vertex(first, {2, -3, 4});
    for (const Vec3f offset : {Vec3f{500, -300, 40}, Vec3f{-12345.5F, 6789.25F, -4321.75F},
                               Vec3f{100000, -250000, 75000}, Vec3f{0, 0, 0}}) {
        const std::string label = "translation " + show(widen(offset));
        RetailCase moved = base;
        moved.eye = {base.eye.x + offset.x, base.eye.y + offset.y, base.eye.z + offset.z};
        moved.target = {base.target.x + offset.x, base.target.y + offset.y, base.target.z + offset.z};
        const auto placement = moved.place();
        expect(placement.status == SunRetailStatus::placed, label + " is placed");
        expect(placement.axes == first.axes && placement.right == first.right && placement.view_up == first.view_up
                   && placement.backward == first.backward && placement.distance == first.distance
                   && placement.scale == first.scale,
               label + " leaves the frame, d and s bit-identical");
        expect_near3(minus(placement.origin, widen(moved.eye)), minus(first.origin, widen(base.eye)),
                     label + ": the sun moves with the eye", 1.0e-6);
        const auto vertex = space::sun_retail_vertex(placement, {2, -3, 4});
        expect(first_vertex && vertex && near3(minus(*vertex, *first_vertex), widen(offset), 1.0e-6),
               label + ": every vertex moves by the eye offset");
    }
    // The eye alone moves the origin; the orientation does not see it.
    RetailCase eye_only = base;
    eye_only.eye = {1010, 20, 5};
    eye_only.target = {970, 70, -15};
    const auto shifted = eye_only.place();
    expect_near3(shifted.origin, {846.03278, 265.95081, 127.97540}, "an eye shifted by +1000 x shifts the origin");
}

void test_camera_rotation() {
    // Fixed eye and L: turning the camera never moves the origin or n; X and Z follow U.
    RetailCase sun;
    sun.eye = {0, 0, 0};
    sun.toward_light = {0.70710677F, 0.70710677F, 0};
    sun.target = {0, 1, 0};
    const auto straight = sun.place();
    for (const Vec3f target : {Vec3f{1, 1, 0}, Vec3f{0, 1, 1}, Vec3f{5, -3, 2}, Vec3f{-1, 0, -0.5F}}) {
        sun.target = target;
        const auto turned = sun.place();
        const std::string label = "camera turned toward " + show(widen(target));
        expect(turned.status == SunRetailStatus::placed && turned.origin == straight.origin
                   && turned.axes[1] == straight.axes[1] && turned.distance == straight.distance,
               label + " keeps the origin and n bit-identical");
    }
    sun.target = {1, 1, 0};
    expect_frame(sun.place(), {0.70711, -0.70711, 0}, {-0.70711, -0.70711, 0}, {0, 0, 1}, "yaw to (1, 1, 0)");
    expect_vertex(sun, {1, 0, 1}, {707.81388, 706.39966, 1}, "yaw to (1, 1, 0) (1, 0, 1)");
    sun.target = {0, 1, 1};
    const auto pitched = sun.place();
    expect_near3(pitched.view_up, {0, -0.70711, 0.70711}, "pitch to (0, 1, 1) U");
    expect_frame(pitched, {0.57735, -0.57735, -0.57735}, {-0.70711, -0.70711, 0}, {0.40825, -0.40825, 0.81650},
                 "pitch to (0, 1, 1)");
    expect_vertex(sun, {1, 0, 1}, {708.09237, 706.12117, 0.23915}, "pitch to (0, 1, 1) (1, 0, 1)");
    expect_vertex(sun, {0, 1, 0}, {706.39966, 706.39966, 0}, "pitch to (0, 1, 1) (0, 1, 0)");

    // Whole-scene rotation: 90 degrees about Z (exact in float) rotates every output.
    const RetailCase base = general_case();
    const auto rz = [](const Vec3f v) { return Vec3f{-v.y, v.x, v.z}; };
    RetailCase turned = base;
    turned.eye = rz(base.eye);
    turned.target = rz(base.target);
    turned.toward_light = rz(base.toward_light);
    const auto rotated = turned.place();
    expect_near3(rotated.origin, {-265.95081, -153.96722, 127.97540}, "RT-07 turned 90 about Z origin");
    expect_frame(rotated, {-0.54363, 0.83875, 0.03106}, {0.76822, 0.51215, -0.38411}, {0.33808, 0.18495, 0.92276},
                 "RT-07 turned 90 about Z");
    expect_vertex(turned, {2, -3, 4}, {-267.99041, -153.08636, 132.88092}, "RT-07 turned 90 about Z (2, -3, 4)");

    // A general rotation Q (axis (1, 2, 3), 40 degrees) of eye, target, up and L.
    const SunVec3 axis = unit3({1, 2, 3});
    const double angle = 40.0 * 3.14159265358979323846 / 180.0;
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const auto q = [&](const SunVec3& v) {
        const SunVec3 k = cross3(axis, v);
        const double kv = dot3(axis, v) * (1 - c);
        return SunVec3{v[0] * c + k[0] * s + axis[0] * kv, v[1] * c + k[1] * s + axis[1] * kv,
                       v[2] * c + k[2] * s + axis[2] * kv};
    };
    RetailCase general_turn = base;
    general_turn.eye = narrow(q(widen(base.eye)));
    general_turn.target = narrow(q(widen(base.target)));
    general_turn.up = narrow(q(widen(base.up)));
    general_turn.toward_light = narrow(q(widen(base.toward_light)));
    const auto expected = base.place();
    const auto actual = general_turn.place();
    expect(actual.status == SunRetailStatus::placed, "Q-rotated RT-07 is placed");
    expect_near3(actual.origin, q(expected.origin), "Q-rotated origin is Q(origin)", 1.0e-3);
    for (std::size_t index = 0; index < 3; ++index) {
        expect_near3(actual.axes[index], q(expected.axes[index]), "Q-rotated axis " + std::to_string(index), 1.0e-5);
    }
}

void test_roll_from_camera_up() {
    // Centred sun: the quad rolls with the camera, so it stays screen-aligned.
    RetailCase centred;
    centred.up = {0.5F, 0, 0.8660254F};
    const auto rolled = centred.place();
    expect_near3(rolled.right, {0.86603, 0, -0.5}, "roll 30 R");
    expect_near3(rolled.view_up, {0.5, 0, 0.86603}, "roll 30 U");
    expect_frame(rolled, {0.86603, 0, -0.5}, {0, -1, 0}, {0.5, 0, 0.86603}, "roll 30 centred");
    expect(near3(rolled.axes[0], rolled.right, 1.0e-9) && near3(rolled.axes[2], rolled.view_up, 1.0e-9),
           "a centred sun maps local +X to R and +Z to U");
    expect_vertex(centred, {1, 0, 1}, {1.36603, 990, 0.36603}, "roll 30 centred (1, 0, 1)");
    centred.up = {1, 0, 0};
    expect_frame(centred.place(), {0, 0, -1}, {0, -1, 0}, {1, 0, 0}, "roll 90 centred");
    expect_vertex(centred, {1, 0, 1}, {1, 990, -1}, "roll 90 centred (1, 0, 1)");
    centred.up = {0, 0, -1};
    expect_frame(centred.place(), {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}, "upside-down camera");
    expect_vertex(centred, {1, 0, 1}, {-1, 990, -1}, "upside-down camera (1, 0, 1)");

    // Off-centre sun: the roll comes from U, and the origin and n do not roll.
    RetailCase off;
    off.toward_light = {0.28221625F, 0.94072086F, 0.18814418F};
    const auto unrolled = off.place();
    expect_near3(unrolled.origin, {282.21625, 930.72086, 188.14418}, "off-centre origin");
    expect_frame(unrolled, {0.95783, -0.28735, 0}, {-0.28222, -0.94072, -0.18814}, {-0.05406, -0.18021, 0.98214},
                 "off-centre unrolled");
    expect_vertex(off, {1, 0, 1}, {283.12001, 930.25330, 189.12632}, "off-centre unrolled (1, 0, 1)");
    off.up = {0.5F, 0, 0.8660254F};
    const auto off_rolled = off.place();
    expect(off_rolled.origin == unrolled.origin && off_rolled.axes[1] == unrolled.axes[1],
           "rolling the camera keeps the origin and n bit-identical");
    expect_frame(off_rolled, {0.85517, -0.15781, -0.49374}, {-0.28222, -0.94072, -0.18814},
                 {0.43478, -0.30024, 0.84902}, "off-centre rolled 30");
    expect_vertex(off, {1, 0, 1}, {283.50620, 930.26281, 188.49946}, "off-centre rolled 30 (1, 0, 1)");

    // The roll is the camera's up axis U, not the raw up hint.
    RetailCase hinted;
    const auto plain = hinted.place();
    hinted.up = {0, 0.3F, 1};
    const auto tilted_hint = hinted.place();
    expect(near3(tilted_hint.axes[0], plain.axes[0], 1.0e-12) && near3(tilted_hint.axes[2], plain.axes[2], 1.0e-12),
           "an up hint tilted toward the view direction gives the same U and the same roll");
    hinted.up = {0, 0, 1000};
    expect(same(hinted.place(), plain), "the up hint's length does not matter");
}

// -- chain facts --------------------------------------------------------------------------

void test_chain_facts() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // Mode: exactly 7; low-four-bit aliases of 7 are named, not placed.
    for (const std::uint32_t mode : {0U, 1U, 5U, 6U, 8U, 15U, 16U, 0xFFFFFFFFU}) {
        RetailCase sun;
        sun.bones[1].billboard = mode;
        expect_status(sun, SunRetailStatus::mode_not_sun, "mode " + std::to_string(mode));
    }
    for (const std::uint32_t mode : {23U, 39U, 0x107U}) {
        RetailCase sun;
        sun.bones[1].billboard = mode;
        expect_status(sun, SunRetailStatus::mode_sun_alias, "authored mode " + std::to_string(mode));
        expect(sun.place().detail.find(std::to_string(mode)) != std::string::npos, "the alias detail names the mode");
    }

    // d comes from the mesh bone's own authored translation only.
    {
        RetailCase sun({0, 0, 0});
        sun.bones[0].relative_transform = translation(1000, 0, 0);
        expect_status(sun, SunRetailStatus::sun_distance_zero, "a far parent does not give d");
        RetailCase tiny({1.0e-6F, 0, 0});
        expect_status(tiny, SunRetailStatus::sun_distance_zero, "d = 1e-6 is zero");
        RetailCase just_over({2.0e-6F, 0, 0});
        const auto placement = just_over.place();
        expect(placement.status == SunRetailStatus::placed && std::abs(placement.distance - 2.0e-6) <= 1.0e-12,
               "d = 2e-6 is placed");
        expect_near3(placement.origin, {0, -10 + 2.0e-6, 0}, "d = 2e-6 origin", 1.0e-9);
        RetailCase huge({3.0e38F, 3.0e38F, 0});
        const auto distant = huge.place();
        expect(distant.status == SunRetailStatus::placed && std::isfinite(distant.origin[1])
                   && std::abs(distant.distance / 4.242640687e38 - 1.0) <= 1.0e-6,
               "a float-overflowing d stays finite in double");
    }

    // Billboard ancestors, whatever their mode.
    for (const std::uint32_t mode : {7U, 1U, 6U, 23U}) {
        RetailCase sun;
        sun.bones[0].billboard = mode;
        expect_status(sun, SunRetailStatus::chain_billboard_ancestor, "parent mode " + std::to_string(mode));
    }
    {
        RetailCase sun;
        sun.bones = {record("Top", -1, 6, identity12), record("Mid", 0, 0, identity12),
                     record("Sun", 1, 7, translation(1000, 0, 0))};
        sun.mesh_bone = 2;
        expect_status(sun, SunRetailStatus::chain_billboard_ancestor, "grandparent mode 6");
        expect(sun.place().detail.find("'Top'") != std::string::npos, "the billboard ancestor is named");
    }

    // Proper rigid chains only, so that s = 1.
    {
        RetailCase sun;
        sun.bones[1].relative_transform = {2, 0, 0, 1000, 0, 2, 0, 0, 0, 0, 2, 0};
        expect_status(sun, SunRetailStatus::chain_not_proper_rigid, "a scaled mesh bone (s would be 2)");
        expect(sun.place().detail.find("'Sun'") != std::string::npos, "the scaled mesh bone is named");
        sun.bones[1].relative_transform = translation(1000, 0, 0);
        sun.bones[0].relative_transform = {0, -2, 0, 0, 2, 0, 0, 50, 0, 0, 2, 0};
        expect_status(sun, SunRetailStatus::chain_not_proper_rigid, "a scaled parent");
        expect(sun.place().detail.find("'Root'") != std::string::npos, "the scaled parent is named");
        sun.bones[0].relative_transform = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0};
        expect_status(sun, SunRetailStatus::chain_not_proper_rigid, "a reflected parent");
        sun.bones[0].relative_transform = {1, 0.5F, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        expect_status(sun, SunRetailStatus::chain_not_proper_rigid, "a sheared parent");
        sun.bones[0].relative_transform = translation(nan, 0, 0);
        expect_status(sun, SunRetailStatus::chain_not_proper_rigid, "a non-finite parent translation");
        sun.bones[0].relative_transform = identity12;
        sun.bones[1].relative_transform = translation(inf, 0, 0);
        expect_status(sun, SunRetailStatus::chain_not_proper_rigid, "a non-finite own translation");
    }

    // Malformed chains.
    for (const std::int32_t index : {-1, -5, 2}) {
        RetailCase sun;
        sun.mesh_bone = index;
        expect_status(sun, SunRetailStatus::chain_invalid, "mesh bone " + std::to_string(index));
    }
    {
        RetailCase self;
        self.bones[1].parent = 1;
        expect_status(self, SunRetailStatus::chain_invalid, "a self-parented sun");
        RetailCase cycle;
        cycle.bones[0].parent = 1;
        expect_status(cycle, SunRetailStatus::chain_invalid, "a two-bone cycle");
        RetailCase out_of_range;
        out_of_range.bones[1].parent = 5;
        expect_status(out_of_range, SunRetailStatus::chain_invalid, "a parent out of range");
        RetailCase negative;
        negative.bones[1].parent = -2;
        expect_status(negative, SunRetailStatus::chain_invalid, "a parent of -2");
        RetailCase root_negative;
        root_negative.bones[0].parent = -2;
        expect_status(root_negative, SunRetailStatus::chain_invalid, "a root parent of -2");
        RetailCase empty;
        empty.bones.clear();
        empty.mesh_bone = 0;
        expect_status(empty, SunRetailStatus::chain_invalid, "no bones");
    }
}

// -- s, the stored matrix's third-column length (R-M7-05) ------------------------------------

// The RT-01 camera and L over `bones`, with the mode-7 mesh bone last.
SunRetailPlacement place_chain(const std::vector<eawr::assets::Bone>& bones) {
    RetailCase sun;
    sun.bones = bones;
    sun.mesh_bone = static_cast<std::int32_t>(bones.size()) - 1;
    return sun.place();
}

void test_stored_scale() {
    // 1.00004F (1.000040054321289) passes the per-bone proper-rigid tolerance.
    const Rigid12 stretched_z{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1.00004F, 0};

    // Review regression: 100 links, each within tolerance, compound to s = 1.00401,
    // not 1. The root is an identity record; links 1..100 scale the third column.
    {
        std::vector<eawr::assets::Bone> bones{record("Root", -1, 0, identity12)};
        for (std::int32_t link = 1; link <= 100; ++link) {
            Rigid12 transform = stretched_z;
            if (link == 100) transform[3] = 1000.0F;
            bones.push_back(record("Link" + std::to_string(link), link - 1, link == 100 ? 7U : 0U, transform));
        }
        const auto placement = place_chain(bones);
        expect(placement.status == SunRetailStatus::placed, "the 100-link chain passes every per-bone check: "
            + placement.detail);
        expect(std::abs(placement.scale - 1.0040133840558638) <= 1.0e-9,
               "the 100-link chain has s = 1.0040134 from the composed stored matrix, got "
                   + std::to_string(placement.scale));
        expect(placement.scale - 1.0 > 4.0e-3, "the 100-link chain's s is not 1");
        RetailCase sun;
        sun.bones = bones;
        sun.mesh_bone = 100;
        expect_vertex(sun, {1000, 0, 0}, {1004.0133840558638, 990, 0}, "100 links: local +X is sized by s", 1.0e-6);
        expect_vertex(sun, {0, 0, 1000}, {0, 990, 1004.0133840558638}, "100 links: local +Z is sized by s", 1.0e-6);
        expect_vertex(sun, {0, 1000, 0}, {0, 990 - 1004.0133840558638, 0}, "100 links: local +Y is sized by s",
                      1.0e-6);
    }

    // The composed column, not the product of per-link column lengths: the parent
    // stretches its second column and the child turns local +Z onto it.
    {
        const std::vector<eawr::assets::Bone> bones{
            record("Root", -1, 0, identity12), record("Parent", 0, 0, {1, 0, 0, 0, 0, 1.00004F, 0, 0, 0, 0, 1, 0}),
            record("Sun", 1, 7, {1, 0, 0, 1000, 0, 0, -1, 0, 0, 1, 0, 0})};
        const auto placement = place_chain(bones);
        expect(std::abs(placement.scale - 1.000040054321289) <= 1.0e-12,
               "a turned child reads its parent's stretched column: s = 1.00004, got "
                   + std::to_string(placement.scale));
        expect_frame(placement, {1, 0, 0}, {0, -1, 0}, {0, 0, 1}, "the turned child's frame is RT-01's");
        RetailCase sun;
        sun.bones = bones;
        sun.mesh_bone = 2;
        expect_vertex(sun, {0, 0, 1000}, {0, 990, 1000.0400543212891}, "turned child: local (0, 0, 1000)", 1.0e-6);
    }

    // The root's authored record is not part of the stored matrix (EV-RET-02).
    {
        const auto scaled_root = place_chain({record("Root", -1, 0, stretched_z),
                                              record("Sun", 0, 7, translation(1000, 0, 0))});
        expect(scaled_root.status == SunRetailStatus::placed && scaled_root.scale == 1.0,
               "a stretched root record does not size the sun");
        Rigid12 lone = stretched_z;
        lone[3] = 1000.0F;
        const auto parentless = place_chain({record("Sun", -1, 7, lone)});
        expect(parentless.status == SunRetailStatus::placed && parentless.scale == 1.0
                   && std::abs(parentless.distance - 1000.0) <= 1.0e-9,
               "a parentless sun has s = 1 (its stored matrix is the world) and keeps its own d");
        const auto own = place_chain({record("Root", -1, 0, identity12), record("Sun", 0, 7, lone)});
        expect(std::abs(own.scale - 1.000040054321289) <= 1.0e-12, "the mesh bone's own record sizes the sun");
    }

    // An exactly rigid chain has s = 1; s never depends on the camera or L.
    {
        const auto plain = RetailCase().place();
        expect(plain.scale == 1.0, "an identity chain has s = 1 exactly");
        RetailCase moved;
        moved.bones[1].relative_transform = stretched_z;
        moved.bones[1].relative_transform[3] = 1000.0F;
        const auto first = moved.place();
        moved.eye = {300, -700, 50};
        moved.target = {310, -690, 45};
        moved.up = {0.2F, 0, 1};
        moved.toward_light = {0.6F, 0, 0.8F};
        const auto second = moved.place();
        expect(first.scale == second.scale && std::abs(first.scale - 1.000040054321289) <= 1.0e-12,
               "s is a chain fact: the camera and L do not change it");
    }
}

} // namespace eawr_sun_retail_test
