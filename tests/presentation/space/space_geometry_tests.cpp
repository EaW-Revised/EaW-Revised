#include "space_test_support.hpp"

namespace eawr_space_test {

void test_camera() {
    const auto valid = space::parse_camera("0,0,0, 0,0,-1, 0,1,0, 60,1,5000", 1280, 720);
    expect(valid.status == space::CameraStatus::valid, "a finite look-at camera parses");
    expect(valid.camera.width == 1280 && valid.camera.far_plane == 5000.0F, "viewport and planes are kept");
    expect(space::parse_camera("0,0,0,0,0,-1,0,1,0,60,1", 1280, 720).status == space::CameraStatus::malformed,
           "eleven values are malformed");
    expect(space::parse_camera("0,0,0,0,0,-1,0,1,0,60,1,5000,7", 1280, 720).status == space::CameraStatus::malformed,
           "thirteen values are malformed");
    expect(space::parse_camera("0,0,x,0,0,-1,0,1,0,60,1,5000", 1280, 720).status == space::CameraStatus::malformed,
           "a non-number is malformed");
    expect(space::parse_camera("nan,0,0,0,0,-1,0,1,0,60,1,5000", 1280, 720).status == space::CameraStatus::nonfinite,
           "a NaN component is nonfinite");
    expect(space::parse_camera("0,0,0,0,0,-1,0,1,0,inf,1,5000", 1280, 720).status == space::CameraStatus::nonfinite,
           "an infinite field of view is nonfinite");
    expect(space::parse_camera("0,0,0,0,0,-1,0,1,0,60,1,-inf", 1280, 720).status == space::CameraStatus::nonfinite,
           "a negative-infinite far plane is nonfinite");
    expect(space::parse_camera("0,0,0,0,0,-1,0,1e999,0,60,1,5000", 1280, 720).status == space::CameraStatus::nonfinite,
           "a float overflow is nonfinite, not silently clamped");
    expect(space::parse_camera("0,0,0,0,0,-1,0,1,0,60,0,5000", 1280, 720).status == space::CameraStatus::near_far_invalid,
           "a zero near plane");
    expect(space::parse_camera("0,0,0,0,0,-1,0,1,0,60,10,5", 1280, 720).status == space::CameraStatus::near_far_invalid,
           "far before near");
    expect(space::parse_camera("0,0,0,0,0,-1,0,1,0,0,1,5000", 1280, 720).status == space::CameraStatus::fov_invalid,
           "a zero field of view");
    expect(space::parse_camera("1,2,3,1,2,3,0,1,0,60,1,5000", 1280, 720).status == space::CameraStatus::direction_degenerate,
           "eye equal to target");
    expect(space::parse_camera("0,0,0,0,-1,0,0,1,0,60,1,5000", 1280, 720).status == space::CameraStatus::up_collinear,
           "up parallel to the view direction");
    expect(space::parse_camera("0,0,0,0,0,-1,0,1,0,60,1,5000", 0, 720).status == space::CameraStatus::viewport_invalid,
           "an empty viewport");
}

void test_projection_and_basis() {
    eawr::presentation::FixedCamera camera;
    camera.width = 100;
    camera.height = 100;
    camera.vertical_fov_degrees = 90.0F;
    camera.near_plane = 1.0F;
    camera.far_plane = 1000.0F;
    camera.eye = {0, 0, 0};
    camera.target = {0, 0, -1};
    camera.up = {0, 1, 0};
    // Source basis: forward is +Y and up is +Z, so this wall at source y=10
    // is render z=-10, straight ahead.
    eawr::assets::Submesh ahead = quad({-5, 10, 5}, {10, 0, 0}, {0, 0, -10}, qualified_shader, "t");
    const auto triangles = space::render_triangles(ahead);
    const auto mask = space::rasterize(camera, triangles);
    expect(mask.count() >= 2400 && mask.count() <= 2600, "a centred 10x10 wall at depth 10 covers the middle half");
    expect(mask.at(50, 50) && !mask.at(10, 50), "coverage is central");
    // The same wall at source y=-10 is behind the camera: frustum-out.
    eawr::assets::Submesh behind = quad({-5, -10, 5}, {10, 0, 0}, {0, 0, -10}, qualified_shader, "t");
    expect(space::rasterize(camera, space::render_triangles(behind)).count() == 0,
           "a surface behind the camera projects nothing");
    // Source +X is screen right; source +Z is screen up. A double or missing
    // conversion would move these regions.
    eawr::assets::Submesh right = quad({2, 10, 1}, {3, 0, 0}, {0, 0, -2}, qualified_shader, "t");
    const auto right_mask = space::rasterize(camera, space::render_triangles(right));
    expect(right_mask.count() > 0 && right_mask.at(68, 50) && !right_mask.at(32, 50), "source +X is screen right");
    eawr::assets::Submesh high = quad({-1, 10, 4}, {2, 0, 0}, {0, 0, -2}, qualified_shader, "t");
    const auto high_mask = space::rasterize(camera, space::render_triangles(high));
    expect(high_mask.count() > 0 && high_mask.at(50, 35) && !high_mask.at(50, 65), "source +Z is screen up");
    // A wall crossing the near plane is clipped, not dropped.
    eawr::assets::Submesh crossing = quad({-5, -5, -2}, {10, 0, 0}, {0, 20, 0}, qualified_shader, "t");
    expect(space::rasterize(camera, space::render_triangles(crossing)).count() > 0,
           "a surface crossing the near plane is clipped and still covers pixels");
    eawr::presentation::FixedCamera bad = camera;
    bad.near_plane = 0.0F;
    expect(space::rasterize(bad, triangles).count() == 0, "an invalid camera projects nothing");

    const auto quadrants = space::render_triangles_by_uv_quadrant(ahead);
    expect(quadrants[0].size() == 2 && quadrants[1].size() == 2 && quadrants[2].size() == 2
               && quadrants[3].size() == 2, "a 2x2-cell quad splits into four UV quadrants");
    // UV (0,0) sits at the source top-left (x=-5, z=+5): quadrant 0 is the
    // screen top-left.
    const auto top_left = space::rasterize(camera, quadrants[0]);
    expect(top_left.at(35, 35) && !top_left.at(65, 65), "UV quadrant 0 projects to the screen top-left");
}

space::ScreenMask rectangle(const std::uint32_t x0, const std::uint32_t y0, const std::uint32_t x1, const std::uint32_t y1) {
    space::ScreenMask mask;
    mask.width = 64;
    mask.height = 48;
    mask.bits.assign(64U * 48U, 0U);
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) mask.bits[y * 64U + x] = 1U;
    }
    return mask;
}

void test_morphology() {
    const auto square = rectangle(10, 10, 20, 20);
    expect(square.count() == 100, "a 10x10 region");
    expect(space::erode(square, 1).count() == 64, "erosion by one gives 8x8");
    expect(space::dilate(square, 1).count() == 144, "dilation by one gives 12x12");
    expect(space::erode(rectangle(0, 0, 10, 10), 1).count() == 64,
           "erosion treats the frame edge as uncovered (81 if it counted as covered)");
    expect(space::unite(square, rectangle(15, 15, 25, 25)).count() == 175, "union");
}

space::Rgb8Image image(const std::uint8_t fill) {
    space::Rgb8Image result;
    result.width = 64;
    result.height = 48;
    result.rgb.assign(64U * 48U * 3U, fill);
    return result;
}

void paint(space::Rgb8Image& target, const space::ScreenMask& mask, const std::uint8_t r, const std::uint8_t g,
           const std::uint8_t b) {
    for (std::size_t index = 0; index < mask.bits.size(); ++index) {
        if (mask.bits[index] == 0U) continue;
        target.rgb[index * 3] = r;
        target.rgb[index * 3 + 1] = g;
        target.rgb[index * 3 + 2] = b;
    }
}

space::SurfaceRegions regions_of(const space::ScreenMask& mask, const std::uint32_t x0, const std::uint32_t x1) {
    space::SurfaceRegions regions;
    regions.mask = mask;
    const std::uint32_t mid = (x0 + x1) / 2;
    regions.quadrants = {rectangle(x0, 2, mid, 15), rectangle(mid, 2, x1, 15), rectangle(x0, 15, mid, 30),
                         rectangle(mid, 15, x1, 30)};
    return regions;
}

void test_pixel_evaluation() {
    const auto left = rectangle(2, 2, 30, 30);
    const auto right = rectangle(34, 2, 62, 30);
    const std::vector<space::SurfaceRegions> regions{regions_of(left, 2, 30), regions_of(right, 34, 62)};
    const auto disabled = image(5);
    auto configured = image(5);
    paint(configured, left, 200, 20, 20);
    paint(configured, right, 20, 20, 200);
    auto only_left = image(5);
    paint(only_left, left, 200, 20, 20);
    auto only_right = image(5);
    paint(only_right, right, 20, 20, 200);
    std::vector<std::optional<space::Rgb8Image>> isolated{only_left, only_right};

    const auto good = space::evaluate_pixels(regions, configured, disabled, isolated, std::nullopt);
    expect(good.status == "verified", "two separated drawn surfaces verify: " + good.failure);
    expect(good.surfaces.size() == 2 && good.surfaces[0].status == space::PixelStatus::verified,
           "per-surface status");
    expect(std::abs(good.surfaces[0].quadrant_mean_rgb[0][0] - 200.0) < 0.01, "quadrant mean colour is measured");
    expect(good.changed_outside == 0, "nothing changes outside the regions");

    // A surface whose interior changes only partly (here about half) is
    // weak evidence: neither verified nor a hard failure.
    auto partial = image(5);
    paint(partial, rectangle(2, 2, 16, 30), 200, 20, 20);
    paint(partial, right, 20, 20, 200);
    auto partial_left = image(5);
    paint(partial_left, rectangle(2, 2, 16, 30), 200, 20, 20);
    std::vector<std::optional<space::Rgb8Image>> partial_isolated{partial_left, only_right};
    const auto weak = space::evaluate_pixels(regions, partial, disabled, partial_isolated, std::nullopt);
    expect(weak.status == "inconclusive" && weak.surfaces.size() == 2
               && weak.surfaces[0].status == space::PixelStatus::inconclusive
               && weak.surfaces[1].status == space::PixelStatus::verified,
           "a partly changed interior is inconclusive, not verified: " + weak.failure);
    expect(weak.surfaces.size() == 2 && weak.surfaces[0].changed_interior > 0
               && weak.surfaces[0].changed_interior * 10 < weak.surfaces[0].interior_pixels * 9,
           "the inconclusive surface changed some but under 90 % of its interior");

    auto leaked = configured;
    paint(leaked, rectangle(0, 44, 64, 48), 255, 255, 255);
    expect(space::evaluate_pixels(regions, leaked, disabled, isolated, std::nullopt).status == "failed",
           "a change outside every region fails");

    std::vector<std::optional<space::Rgb8Image>> dropped{only_left, std::nullopt};
    auto removed = space::evaluate_pixels(regions, configured, disabled, dropped, std::nullopt);
    expect(removed.status == "failed" && removed.surfaces[1].status == space::PixelStatus::not_submitted,
           "a removed submission invalidates that surface's evidence");

    auto unchanged = image(5);
    paint(unchanged, left, 200, 20, 20);
    auto blank = space::evaluate_pixels(regions, unchanged, disabled, isolated, std::nullopt);
    expect(blank.status == "failed" && blank.surfaces[1].status == space::PixelStatus::no_change,
           "a surface that drew nothing fails rather than passing on another surface's pixels");

    auto spill = only_right;
    paint(spill, rectangle(2, 2, 30, 30), 90, 90, 90);
    std::vector<std::optional<space::Rgb8Image>> spilled{only_left, spill};
    expect(space::evaluate_pixels(regions, configured, disabled, spilled, std::nullopt).surfaces[1].status
               == space::PixelStatus::leaked_outside_region, "isolation attributes pixels to their own surface");

    // A foreground occluder covering part of the left surface: the sky must
    // not change pixels under it.
    const auto occluder = rectangle(8, 8, 20, 20);
    auto with_occluder = configured;
    paint(with_occluder, occluder, 0, 200, 0);
    auto disabled_occluder = disabled;
    paint(disabled_occluder, occluder, 0, 200, 0);
    auto only_left_occluded = only_left;
    paint(only_left_occluded, occluder, 0, 200, 0);
    auto only_right_occluded = only_right;
    paint(only_right_occluded, occluder, 0, 200, 0);
    std::vector<std::optional<space::Rgb8Image>> occluded_isolated{only_left_occluded, only_right_occluded};
    auto occluded = space::evaluate_pixels(regions, with_occluder, disabled_occluder, occluded_isolated, occluder);
    expect(occluded.status == "verified" && occluded.occlusion_status == "verified",
           "the foreground occluder is unchanged by the sky: " + occluded.failure);
    auto overdrawn = space::evaluate_pixels(regions, configured, disabled_occluder, occluded_isolated, occluder);
    expect(overdrawn.occlusion_status == "failed", "sky drawn over the occluder fails depth occlusion");
    const auto full = rectangle(0, 0, 32, 48);
    auto covered = space::evaluate_pixels(regions, with_occluder, disabled_occluder, occluded_isolated, full);
    expect(covered.surfaces[0].status == space::PixelStatus::occluded, "a fully occluded surface is not verified");

    const std::vector<space::SurfaceRegions> off_camera{space::SurfaceRegions{rectangle(0, 0, 0, 0), {}}};
    std::vector<std::optional<space::Rgb8Image>> single{disabled};
    auto nothing = space::evaluate_pixels(off_camera, disabled, disabled, single, std::nullopt);
    expect(nothing.status == "failed" && nothing.surfaces[0].status == space::PixelStatus::not_projected,
           "an off-camera surface is not projected");

    space::Rgb8Image small;
    small.width = 2;
    small.height = 2;
    small.rgb.assign(12, 0);
    expect(space::evaluate_pixels(regions, small, disabled, isolated, std::nullopt).status == "failed",
           "a capture of the wrong size fails");
}

// -- mode-7 sun reference geometry -------------------------------------------------
//
// docs/behaviour/meshadditive-sun-billboard.md T-01..T-10 and R-01..R-06. Every
// expected value below is hard-coded from an independent double-precision
// derivation (see docs/rendering.md#sun-policies); none is
// computed by the evaluator under test.

using space::SunPlacementStatus;
using space::SunVec3;
using Rigid12 = std::array<float, 12>;

constexpr Rigid12 identity12{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

bool near3(const SunVec3& a, const SunVec3& b, const double tolerance = 1.0e-3) {
    return std::abs(a[0] - b[0]) <= tolerance && std::abs(a[1] - b[1]) <= tolerance
        && std::abs(a[2] - b[2]) <= tolerance;
}

std::string show(const SunVec3& value) {
    return "(" + std::to_string(value[0]) + ", " + std::to_string(value[1]) + ", " + std::to_string(value[2]) + ")";
}

eawr::assets::Bone sun_record(const std::string& name, const std::int32_t parent, const std::uint32_t billboard,
                              const Rigid12& transform) {
    eawr::assets::Bone bone;
    bone.name = name;
    bone.parent = parent;
    bone.billboard = billboard;
    bone.relative_transform = transform;
    return bone;
}

Rigid12 translation(const float x, const float y, const float z) { return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z}; }

// An identity root and a mode-7 child translated to `origin`, under the T-01
// camera and L = +X.
struct SunCase final {
    std::vector<eawr::assets::Bone> bones;
    std::int32_t mesh_bone{1};
    Vec3f eye{0.0F, -10.0F, 0.0F};
    Vec3f target{0.0F, 0.0F, 0.0F};
    Vec3f up{0.0F, 0.0F, 1.0F};
    Vec3f toward_sun{1.0F, 0.0F, 0.0F};

    explicit SunCase(const Vec3f origin = {}) {
        bones.push_back(sun_record("Root", -1, 0, identity12));
        bones.push_back(sun_record("Sun", 0, 7, translation(origin.x, origin.y, origin.z)));
    }
    [[nodiscard]] space::SunReferenceInput input() const {
        space::SunReferenceInput value;
        value.bones = bones;
        value.mesh_bone = mesh_bone;
        value.eye = eye;
        value.target = target;
        value.up = up;
        value.toward_sun = toward_sun;
        return value;
    }
    [[nodiscard]] space::SunReferencePlacement place() const { return space::sun_reference_placement(input()); }
};

bool all_zero(const space::SunReferencePlacement& placement) {
    const SunVec3 zero{};
    return placement.rest_origin == zero && placement.sun_origin == zero && placement.right == zero
        && placement.view_up == zero && placement.backward == zero && placement.sun_axes[0] == zero
        && placement.sun_axes[1] == zero && placement.sun_axes[2] == zero && placement.tilt == 0.0
        && placement.azimuth == 0.0;
}

void expect_sun_status(const SunCase& sun, const SunPlacementStatus status, const std::string& label) {
    const auto placement = sun.place();
    expect(placement.status == status, label + ": expected " + std::string(space::to_string(status)) + ", got "
        + std::string(space::to_string(placement.status)) + " (" + placement.detail + ")");
    if (status == SunPlacementStatus::placed) return;
    expect(!placement.detail.empty(), label + ": a failure names its cause");
    expect(all_zero(placement), label + ": a failure carries no geometry");
    expect(!space::sun_reference_vertex(placement, {0.0F, 0.0F, 0.0F}), label + ": a failure places no vertex");
}

void expect_sun_vertex(const SunCase& sun, const Vec3f& local, const SunVec3& expected, const std::string& label) {
    const auto placement = sun.place();
    const auto vertex = space::sun_reference_vertex(placement, local);
    expect(placement.status == SunPlacementStatus::placed && vertex && near3(*vertex, expected),
           label + ": expected " + show(expected) + ", got "
               + (vertex ? show(*vertex) : std::string(space::to_string(placement.status))));
}

void expect_near3(const SunVec3& actual, const SunVec3& expected, const std::string& label) {
    expect(near3(actual, expected), label + ": expected " + show(expected) + ", got " + show(actual));
}

namespace {

void sun_basis_cases() {
    // T-01 basis check.
    {
        const SunCase sun;
        const auto placement = sun.place();
        expect(placement.status == SunPlacementStatus::placed && placement.detail.empty(), "T-01 is placed");
        expect_near3(placement.right, {1, 0, 0}, "T-01 R");
        expect_near3(placement.view_up, {0, 0, 1}, "T-01 U");
        expect_near3(placement.backward, {0, -1, 0}, "T-01 B");
        expect_near3(placement.sun_axes[0], {1, 0, 0}, "T-01 S(+X)");
        expect_near3(placement.sun_axes[1], {0, 1, 0}, "T-01 S(+Y)");
        expect_near3(placement.sun_axes[2], {0, 0, 1}, "T-01 S(+Z)");
        expect(std::abs(placement.tilt) <= 1.0e-9 && std::abs(placement.azimuth) <= 1.0e-9, "T-01 tilt and azimuth 0");
        expect_sun_vertex(sun, {1, 0, 1}, {1, 0, 1}, "T-01 (1, 0, 1)");
        expect_sun_vertex(sun, {1, 0, -1}, {1, 0, -1}, "T-01 (1, 0, -1)");
        expect_sun_vertex(sun, {0, -1, 0}, {0, -1, 0}, "T-01 local -Y faces the eye");
    }

    // T-02 camera translation: the eye position does not place or orient.
    {
        SunCase sun({1000, 0, 0});
        expect_sun_vertex(sun, {1, 0, 1}, {1001, 0, 1}, "T-02 first camera");
        sun.eye = {500, -300, 40};
        sun.target = {500, -290, 40};
        expect_sun_vertex(sun, {1, 0, 1}, {1001, 0, 1}, "T-02 translated camera");
        expect_near3(sun.place().backward, {0, -1, 0}, "T-02 B is unchanged by translation");
    }

    // T-03 camera rotation.
    {
        SunCase sun;
        sun.eye = {0, 0, 0};
        sun.target = {1, 0, 0};
        const auto placement = sun.place();
        expect_near3(placement.right, {0, -1, 0}, "T-03a R");
        expect_near3(placement.view_up, {0, 0, 1}, "T-03a U");
        expect_near3(placement.backward, {-1, 0, 0}, "T-03a B");
        expect_sun_vertex(sun, {1, 0, 1}, {0, -1, 1}, "T-03a (1, 0, 1)");
        expect_sun_vertex(sun, {0, -1, 0}, {-1, 0, 0}, "T-03a local -Y");
        sun.target = {1, 1, 1};
        const auto diagonal = sun.place();
        expect_near3(diagonal.right, {0.70711, -0.70711, 0}, "T-03b R");
        expect_near3(diagonal.view_up, {-0.40825, -0.40825, 0.81650}, "T-03b U");
        expect_near3(diagonal.backward, {-0.57735, -0.57735, -0.57735}, "T-03b B");
        expect_sun_vertex(sun, {1, 0, 1}, {0.29886, -1.11536, 0.81650}, "T-03b (1, 0, 1)");
    }

    // T-04 sun rotation; the magnitude of L does not matter.
    for (const Vec3f toward : {Vec3f{0, 1, 1}, Vec3f{0, 2, 2}}) {
        const std::string label = "T-04 L = (0, " + std::to_string(toward.y) + ", " + std::to_string(toward.z) + ")";
        SunCase sun({1000, 0, 0});
        sun.toward_sun = toward;
        const auto placement = sun.place();
        expect_near3(placement.sun_axes[0], {0, 0.70711, 0.70711}, label + " S(+X)");
        expect_near3(placement.sun_axes[1], {-1, 0, 0}, label + " S(+Y)");
        expect_near3(placement.sun_axes[2], {0, -0.70711, 0.70711}, label + " S(+Z)");
        expect(std::abs(placement.tilt - 0.78540) <= 1.0e-5 && std::abs(placement.azimuth - 1.57080) <= 1.0e-5,
               label + " tilt pi/4, azimuth pi/2");
        expect_near3(placement.sun_origin, {0, 707.10678, 707.10678}, label + " o = (1000, 0, 0)");
        SunCase offset({1000, 0, 10});
        offset.toward_sun = toward;
        const auto off = offset.place();
        expect_near3(off.rest_origin, {1000, 0, 10}, label + " rest origin");
        expect_near3(off.sun_origin, {0, 700.03571, 714.17785}, label + " o = (1000, 0, 10)");
        expect(!near3(off.sun_origin, {0, 707.1421, 707.1421}), label + " is not |o| * L");
    }
}

void sun_parent_cases() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // T-05 rotated parent (12-float records in Bone::relative_transform layout).
    const Rigid12 t05_parent{0, -1, 0, 0, 1, 0, 0, 50, 0, 0, 1, 0};
    const Rigid12 t05_child{1, 0, 0, 1000, 0, 0.8660254F, -0.5F, 0, 0, 0.5F, 0.8660254F, 0};
    {
        SunCase sun;
        sun.bones = {sun_record("Parent", -1, 0, t05_parent), sun_record("Sun", 0, 7, t05_child)};
        const auto placement = sun.place();
        expect_near3(placement.rest_origin, {0, 1050, 0}, "T-05a rest origin");
        expect_near3(placement.sun_origin, {0, 1050, 0}, "T-05a sun origin");
        expect_sun_vertex(sun, {1, 0, 1}, {1, 1050, 1}, "T-05a (1, 0, 1)");
        // The mesh bone's own rotation changes nothing.
        SunCase unrotated = sun;
        unrotated.bones[1].relative_transform = translation(1000, 0, 0);
        const auto plain = unrotated.place();
        const auto rotated_vertex = space::sun_reference_vertex(placement, {1, 0, 1});
        const auto plain_vertex = space::sun_reference_vertex(plain, {1, 0, 1});
        expect(rotated_vertex && plain_vertex && near3(*rotated_vertex, *plain_vertex, 1.0e-9),
               "T-05a the mesh bone's rotation does not orient the mesh");
    }
    {
        SunCase sun;
        sun.bones = {sun_record("Parent", -1, 0, t05_parent),
                     sun_record("Sun", 0, 7, {2, 0, 0, 1000, 0, 1.7320508F, -1, 0, 0, 1, 1.7320508F, 0})};
        expect_sun_status(sun, SunPlacementStatus::chain_not_proper_rigid, "T-05b scaled mesh bone");
        expect(sun.place().detail.find("'Sun'") != std::string::npos, "T-05b names the scaled bone");
        sun.bones = {sun_record("Parent", -1, 0, {0, -2, 0, 0, 2, 0, 0, 50, 0, 0, 2, 0}),
                     sun_record("Sun", 0, 7, t05_child)};
        expect_sun_status(sun, SunPlacementStatus::chain_not_proper_rigid, "T-05c scaled parent");
        expect(sun.place().detail.find("'Parent'") != std::string::npos, "T-05c names the scaled parent");
        sun.bones[0].relative_transform = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0};
        expect_sun_status(sun, SunPlacementStatus::chain_not_proper_rigid, "a reflected parent");
        sun.bones[0].relative_transform = {1, 0.5F, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        expect_sun_status(sun, SunPlacementStatus::chain_not_proper_rigid, "a sheared parent");
        sun.bones[0].relative_transform = translation(nan, 0, 0);
        expect_sun_status(sun, SunPlacementStatus::chain_not_proper_rigid, "a non-finite parent translation");
    }
}

void sun_direction_cases() {
    // T-06 opposite, vertical and zero L.
    {
        SunCase sun({-1000, 0, 0});
        sun.toward_sun = {-1, 0, 0};
        expect_near3(sun.place().sun_origin, {1000, 0, 0}, "T-06a opposite L");
        SunCase vertical({1000, 0, 10});
        for (const Vec3f toward : {Vec3f{0, 0, 1}, Vec3f{-0.0F, -0.0F, 1}, Vec3f{1.0e-9F, 0, 1},
                                   Vec3f{-1.0e-9F, 0, 1}, Vec3f{0, 0, -1}, Vec3f{0, 1.0e-6F, 1}, Vec3f{0, 0, 1.0e-30F}}) {
            vertical.toward_sun = toward;
            expect_sun_status(vertical, SunPlacementStatus::sun_direction_vertical,
                              "T-06b L = (" + std::to_string(toward.x) + ", " + std::to_string(toward.y) + ", "
                                  + std::to_string(toward.z) + ")");
        }
        // Just past the 1e-6 threshold is placed, with no jump.
        vertical.toward_sun = {2.0e-6F, 0, 1};
        expect_near3(vertical.place().sun_origin, {-9.998, 0, 1000.00002}, "T-06b L just past the vertical threshold");
        for (const Vec3f toward : {Vec3f{0, 0, 0}, Vec3f{-0.0F, -0.0F, -0.0F}, Vec3f{0, -0.0F, 0}}) {
            vertical.toward_sun = toward;
            expect_sun_status(vertical, SunPlacementStatus::sun_direction_zero, "T-06c zero L");
        }
    }

    // T-07 rotated camera, non-axis L and off-axis origin together.
    {
        SunCase sun({1000, 0, 10});
        sun.eye = {0, 0, 0};
        sun.target = {1, 1, 1};
        sun.toward_sun = {0, 1, 1};
        expect_sun_vertex(sun, {1, 0, 1}, {0.29886, 698.92036, 714.99435}, "T-07 (1, 0, 1)");
        expect_sun_vertex(sun, {0, -1, 0}, {-0.57735, 699.45836, 713.60050}, "T-07 local -Y");
        expect(!near3(sun.place().sun_origin, {7.07107, -1000, 7.07107}), "T-07 is not the transposed S");
    }

    // T-08 S(+Y) alone.
    {
        SunCase sun({0, 100, 0});
        sun.toward_sun = {0, 1, 1};
        expect_near3(sun.place().sun_origin, {-100, 0, 0}, "T-08 S(+Y)");
    }

    // T-09 double policy: a float-overflowing |L| is still placed by its angles.
    {
        SunCase sun({1000, 0, 0});
        sun.toward_sun = {3.0e38F, 0, 3.0e38F};
        expect_near3(sun.place().sun_origin, {707.10678, 0, 707.10678}, "T-09 L = (3e38, 0, 3e38)");
    }
}

// T-10 general case: rotated parent, off-axis origin, oblique camera and L.
SunCase general_sun_case() {
    SunCase general;
    general.bones = {sun_record("Parent", -1, 0, {0, -1, 0, 20, 1, 0, 0, -40, 0, 0, 1, 5}),
                     sun_record("Sun", 0, 7, {1, 0, 0, -300, 0, 0.8660254F, -0.5F, -100, 0, 0.5F, 0.8660254F, 50})};
    general.eye = {10, 20, 5};
    general.target = {-30, 70, -15};
    general.toward_sun = {-2, 3, 1.5F};
    return general;
}

void sun_general_cases(const SunCase& general) {
    const auto placement = general.place();
    expect(placement.status == SunPlacementStatus::placed, "T-10 is placed");
    expect_near3(placement.rest_origin, {120, -340, 55}, "T-10 rest origin");
    expect_near3(placement.sun_axes[0], {-0.51215, 0.76822, 0.38411}, "T-10 S(+X)");
    expect_near3(placement.sun_axes[1], {-0.83205, -0.55470, 0}, "T-10 S(+Y)");
    expect_near3(placement.sun_axes[2], {0.21307, -0.31960, 0.92329}, "T-10 S(+Z)");
    expect_near3(placement.sun_origin, {233.15804, 263.20665, 96.87407}, "T-10 S(o)");
    expect_near3(placement.right, {0.78087, 0.62470, 0}, "T-10 R");
    expect_near3(placement.view_up, {-0.18625, 0.23281, 0.95452}, "T-10 U");
    expect_near3(placement.backward, {0.59628, -0.74536, 0.29814}, "T-10 B");
    expect(std::abs(placement.tilt - 0.39424) <= 1.0e-5 && std::abs(placement.azimuth - 2.15880) <= 1.0e-5,
           "T-10 tilt and azimuth");
    expect_sun_vertex(general, {2, -3, 4}, {235.76364, 263.15122, 101.58658}, "T-10 (2, -3, 4)");
}

void sun_chain_rejection_cases() {
    // R-01 exact mode 7 only.
    for (const std::uint32_t mode : {0U, 1U, 5U, 6U, 8U, 0xFFFFFFFFU}) {
        SunCase sun;
        sun.bones[1].billboard = mode;
        expect_sun_status(sun, SunPlacementStatus::mode_not_sun, "R-01 mode " + std::to_string(mode));
    }

    // R-02 any billboard ancestor.
    for (const std::uint32_t mode : {7U, 1U, 6U}) {
        SunCase sun;
        sun.bones[0].billboard = mode;
        expect_sun_status(sun, SunPlacementStatus::chain_billboard_ancestor, "R-02 parent mode " + std::to_string(mode));
    }
    {
        SunCase sun;
        sun.bones = {sun_record("Top", -1, 6, identity12), sun_record("Mid", 0, 0, identity12),
                     sun_record("Sun", 1, 7, translation(1000, 0, 0))};
        sun.mesh_bone = 2;
        expect_sun_status(sun, SunPlacementStatus::chain_billboard_ancestor, "R-02 grandparent mode 6");
        expect(sun.place().detail.find("'Top'") != std::string::npos, "R-02 names the billboard ancestor");
    }

    // R-03 malformed chains.
    for (const std::int32_t index : {-1, -5, 2}) {
        SunCase sun;
        sun.mesh_bone = index;
        expect_sun_status(sun, SunPlacementStatus::chain_invalid, "R-03 mesh bone " + std::to_string(index));
    }
    {
        SunCase self;
        self.bones[1].parent = 1;
        expect_sun_status(self, SunPlacementStatus::chain_invalid, "R-03 self-parented sun");
        SunCase cycle;
        cycle.bones[0].parent = 1;
        expect_sun_status(cycle, SunPlacementStatus::chain_invalid, "R-03 two-bone cycle");
        SunCase out_of_range;
        out_of_range.bones[1].parent = 5;
        expect_sun_status(out_of_range, SunPlacementStatus::chain_invalid, "R-03 parent out of range");
        SunCase negative;
        negative.bones[1].parent = -2;
        expect_sun_status(negative, SunPlacementStatus::chain_invalid, "R-03 parent -2");
        SunCase root_negative;
        root_negative.bones[0].parent = -2;
        expect_sun_status(root_negative, SunPlacementStatus::chain_invalid, "R-03 root parent -2");
        SunCase empty;
        empty.bones.clear();
        empty.mesh_bone = 0;
        expect_sun_status(empty, SunPlacementStatus::chain_invalid, "R-03 no bones");
        SunCase lone;
        lone.bones.erase(lone.bones.begin());
        lone.bones[0].parent = -1;
        lone.mesh_bone = 0;
        expect_sun_vertex(lone, {1, 0, 1}, {1, 0, 1}, "a parentless mode-7 bone at the origin is placed");
    }
}

void sun_input_rejection_cases() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    // R-04 non-finite camera and L.
    for (const Vec3f toward : {Vec3f{nan, 0, 1}, Vec3f{1, inf, 0}, Vec3f{0, 0, -inf}}) {
        SunCase sun;
        sun.toward_sun = toward;
        expect_sun_status(sun, SunPlacementStatus::sun_direction_nonfinite, "R-04 non-finite L");
    }
    for (int field = 0; field < 3; ++field) {
        SunCase sun;
        (field == 0 ? sun.eye : field == 1 ? sun.target : sun.up).y = field == 1 ? inf : nan;
        expect_sun_status(sun, SunPlacementStatus::camera_nonfinite, "R-04 non-finite camera field " + std::to_string(field));
    }

    // R-05 degenerate camera, at validate_camera's thresholds.
    {
        SunCase sun;
        sun.target = sun.eye;
        expect_sun_status(sun, SunPlacementStatus::camera_direction_degenerate, "R-05 eye = target");
        sun.eye = {0, 0, 0};
        sun.target = {5.0e-7F, 0, 0};
        expect_sun_status(sun, SunPlacementStatus::camera_direction_degenerate, "R-05 eye 5e-7 from target");
        SunCase overhead;
        overhead.eye = {0, 0, 10};
        expect_sun_status(overhead, SunPlacementStatus::camera_up_collinear, "R-05 looking straight down +Z up");
        SunCase zero_up;
        zero_up.up = {0, 0, 0};
        expect_sun_status(zero_up, SunPlacementStatus::camera_up_collinear, "R-05 zero up");
        zero_up.up = {0, 0, 5.0e-7F};
        expect_sun_status(zero_up, SunPlacementStatus::camera_up_collinear, "R-05 up shorter than 1e-6");
        SunCase grazing;
        grazing.up = {0, 1, 1.0e-5F};
        expect_sun_status(grazing, SunPlacementStatus::camera_up_collinear, "R-05 up within 1e-4 of forward");
        grazing.up = {0, 1, 1.0e-3F};
        expect_sun_status(grazing, SunPlacementStatus::placed, "R-05 up 1e-3 off forward is placed");
        expect_near3(grazing.place().view_up, {0, 0, 1}, "R-05 grazing up still gives U = +Z");
        SunCase scaled_up;
        scaled_up.up = {0, 1000, 0.05F};
        expect_sun_status(scaled_up, SunPlacementStatus::camera_up_collinear,
                          "R-05 up collinearity uses a normalised sine, not raw cross length");
    }

    // R-05 L verticality is relative to |L|, including small finite vectors.
    {
        SunCase large_vertical;
        large_vertical.toward_sun = {1, 0, 1.0e7F};
        expect_sun_status(large_vertical, SunPlacementStatus::sun_direction_vertical,
                          "R-05 large near-vertical L is rejected by horizontal ratio");
        SunCase small_oblique;
        small_oblique.toward_sun = {1.0e-7F, 0, 1.0e-3F};
        expect_sun_status(small_oblique, SunPlacementStatus::placed,
                          "R-05 small oblique L is accepted by horizontal ratio");
    }

    // R-06 vertex validation.
    {
        const auto placement = SunCase().place();
        expect(!space::sun_reference_vertex(placement, {nan, 0, 0}), "R-06 NaN local x");
        expect(!space::sun_reference_vertex(placement, {0, 0, inf}), "R-06 infinite local z");
        expect(space::sun_reference_vertex(placement, {0, 0, 0}).has_value(), "R-06 a finite local vertex is placed");
    }
}

void sun_precedence_cases() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // Failure precedence: mode, chain (structure, ancestor, rigidity), camera, L.
    {
        SunCase sun;
        sun.bones[1].billboard = 0;
        sun.bones[0].parent = 9;
        sun.eye.x = nan;
        sun.toward_sun = {0, 0, 0};
        expect_sun_status(sun, SunPlacementStatus::mode_not_sun, "precedence: mode before everything");
        sun.mesh_bone = 4;
        expect_sun_status(sun, SunPlacementStatus::chain_invalid, "precedence: an unreadable mesh bone first");
        sun.mesh_bone = 1;
        sun.bones[1].billboard = 7;
        sun.bones[0].billboard = 1;
        expect_sun_status(sun, SunPlacementStatus::chain_invalid, "precedence: structure before ancestor mode");
        sun.bones[0].parent = -1;
        sun.bones[1].relative_transform[0] = 2.0F;
        expect_sun_status(sun, SunPlacementStatus::chain_billboard_ancestor,
                          "precedence: ancestor mode before a nearer scaled bone");
        sun.bones[0].billboard = 0;
        expect_sun_status(sun, SunPlacementStatus::chain_not_proper_rigid, "precedence: chain before camera");
        sun.bones[1].relative_transform[0] = 1.0F;
        expect_sun_status(sun, SunPlacementStatus::camera_nonfinite, "precedence: camera before L");
        sun.eye = {0, 0, 0};
        sun.toward_sun = {nan, 0, 0};
        expect_sun_status(sun, SunPlacementStatus::camera_direction_degenerate, "precedence: degenerate camera before L");
        sun.eye = {0, 0, 10};
        sun.toward_sun = {0, 0, 1};
        expect_sun_status(sun, SunPlacementStatus::camera_up_collinear, "precedence: collinear up before vertical L");
        sun.eye = {0, -10, 0};
        sun.toward_sun = {0, 0, nan};
        expect_sun_status(sun, SunPlacementStatus::sun_direction_nonfinite, "precedence: non-finite L before zero");
    }
}

bool same_placement(const space::SunReferencePlacement& a, const space::SunReferencePlacement& b) {
    return a.status == b.status && a.detail == b.detail && a.rest_origin == b.rest_origin
        && a.sun_origin == b.sun_origin && a.right == b.right && a.view_up == b.view_up
        && a.backward == b.backward && a.sun_axes == b.sun_axes && a.tilt == b.tilt && a.azimuth == b.azimuth;
}

void sun_repeatability_cases(const SunCase& general) {
    // Source immutability, repeatability, and visibility left to the planner.
    {
        const auto before = general.bones;
        const auto first = general.place();
        const auto second = general.place();
        bool unchanged = before.size() == general.bones.size();
        for (std::size_t index = 0; unchanged && index < before.size(); ++index) {
            const auto& a = before[index];
            const auto& b = general.bones[index];
            unchanged = a.name == b.name && a.parent == b.parent && a.visible == b.visible
                && a.billboard == b.billboard && a.relative_transform == b.relative_transform;
        }
        expect(unchanged, "the bind records are not modified");
        expect(same_placement(first, second), "repeated evaluation is bit-identical");
        SunCase hidden = general;
        hidden.bones[0].visible = false;
        hidden.bones[1].visible = false;
        expect(same_placement(first, hidden.place()), "bone visibility is not read by the evaluator");
    }
}

void sun_render_basis_cases() {
    // Render -> source conversion for a FixedCamera.
    {
        const eawr::presentation::FixedCamera camera;
        const Vec3f eye = space::source_from_render(camera.eye);
        const Vec3f up = space::source_from_render(camera.up);
        expect(eye.x == 0.0F && eye.y == -1050.0F && eye.z == 420.0F, "render eye (0, 420, 1050) is source (0, -1050, 420)");
        expect(up.x == 0.0F && up.y == 0.0F && up.z == 1.0F, "render up +Y is source +Z");
        const auto submesh = quad({400, -100, 100}, {0, 200, 0}, {0, 0, -200}, qualified_shader, "");
        const auto triangles = space::render_triangles(submesh);
        bool round_trip = !triangles.empty();
        for (std::size_t index = 0; round_trip && index < 3; ++index) {
            const auto& render = triangles.front()[index];
            const Vec3f back = space::source_from_render({render.x, render.y, render.z});
            const Vec3f original = submesh.vertices[submesh.indices[index]].position;
            round_trip = back.x == original.x && back.y == original.y && back.z == original.z;
        }
        expect(round_trip, "source_from_render inverts the upload conversion exactly");
        SunCase rendered;
        rendered.eye = space::source_from_render({0, 0, 10});
        rendered.target = space::source_from_render({0, 0, 0});
        rendered.up = space::source_from_render({0, 1, 0});
        expect_sun_vertex(rendered, {1, 0, 1}, {1, 0, 1}, "the T-01 camera given in the render basis");
    }
}

void sun_planner_cases() {
    // The planner is unchanged: a sun the evaluator places is still rejected.
    for (const std::uint32_t mode : {1U, 6U, 7U, 8U}) {
        eawr::assets::Model model = sky_model();
        model.bones.push_back(sun_record("Sun", 0, mode, translation(1000, 0, 0)));
        model.meshes[1].bone = 1;
        if (mode == 7) {
            space::SunReferenceInput input;
            input.bones = model.bones;
            input.mesh_bone = 1;
            input.eye = {0, -10, 0};
            input.toward_sun = {1, 0, 0};
            expect(space::sun_reference_placement(input).status == SunPlacementStatus::placed,
                   "the planner fixture's sun is placed by the evaluator");
        }
        const auto diffuse = space::plan_surfaces(model, space::qualifications());
        const auto routed = space::plan_surfaces(model, space::qualifications(), space::meshadditive_material_routes());
        for (const auto* surfaces : {&diffuse, &routed}) {
            expect(surfaces->size() == 2 && (*surfaces)[0].status == space::SurfaceStatus::accepted
                       && (*surfaces)[1].status == space::SurfaceStatus::hierarchy_unsupported && !(*surfaces)[1].model,
                   "planner verdict for a mode-" + std::to_string(mode) + " bone is unchanged");
        }
    }
}

} // namespace

void test_sun_reference_geometry() {
    expect(space::sun_reference_policy_id == "eawr-sun-mode7-reference-av01-v1", "the sun policy id is pinned");
    sun_basis_cases();
    sun_parent_cases();
    sun_direction_cases();
    const SunCase general = general_sun_case();
    sun_general_cases(general);
    sun_chain_rejection_cases();
    sun_input_rejection_cases();
    sun_precedence_cases();
    sun_repeatability_cases(general);
    sun_render_basis_cases();
    sun_planner_cases();
}

} // namespace eawr_space_test
