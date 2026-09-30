#include "eawr/presentation/space/space.hpp"

#include "space_internal.hpp"

#include "eawr/presentation/terrain/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace eawr::presentation::space {
// -- camera ---------------------------------------------------------------------

namespace {

struct V3 final {
    double x{}, y{}, z{};
};

[[nodiscard]] V3 sub(const V3& a, const V3& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] double dot(const V3& a, const V3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] V3 cross(const V3& a, const V3& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] double length(const V3& a) noexcept { return std::sqrt(dot(a, a)); }
[[nodiscard]] V3 scale(const V3& a, const double factor) noexcept { return {a.x * factor, a.y * factor, a.z * factor}; }
[[nodiscard]] V3 from(const std::array<float, 3>& value) noexcept { return {value[0], value[1], value[2]}; }
[[nodiscard]] V3 from(const assets::Vec3f& value) noexcept { return {value.x, value.y, value.z}; }

} // namespace

std::string_view to_string(const CameraStatus status) noexcept {
    switch (status) {
    case CameraStatus::valid: return "valid";
    case CameraStatus::malformed: return "malformed";
    case CameraStatus::nonfinite: return "nonfinite";
    case CameraStatus::viewport_invalid: return "viewport_invalid";
    case CameraStatus::fov_invalid: return "fov_invalid";
    case CameraStatus::near_far_invalid: return "near_far_invalid";
    case CameraStatus::direction_degenerate: return "direction_degenerate";
    case CameraStatus::up_collinear: return "up_collinear";
    }
    return "invalid";
}

CameraStatus validate_camera(const FixedCamera& camera) noexcept {
    const auto all_finite = [](const std::array<float, 3>& value) {
        return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
    };
    if (!all_finite(camera.eye) || !all_finite(camera.target) || !all_finite(camera.up)
        || !std::isfinite(camera.vertical_fov_degrees) || !std::isfinite(camera.near_plane)
        || !std::isfinite(camera.far_plane)) {
        return CameraStatus::nonfinite;
    }
    if (camera.width == 0 || camera.height == 0 || camera.width > 16384 || camera.height > 16384) {
        return CameraStatus::viewport_invalid;
    }
    if (camera.vertical_fov_degrees < 1.0F || camera.vertical_fov_degrees > 179.0F) return CameraStatus::fov_invalid;
    if (camera.near_plane <= 0.0F || camera.far_plane <= camera.near_plane) return CameraStatus::near_far_invalid;
    const V3 forward = sub(from(camera.target), from(camera.eye));
    const double distance = length(forward);
    if (!(distance > 1.0e-6)) return CameraStatus::direction_degenerate;
    const V3 up = from(camera.up);
    const double up_length = length(up);
    if (!(up_length > 1.0e-6)) return CameraStatus::up_collinear;
    if (length(cross(scale(forward, 1.0 / distance), scale(up, 1.0 / up_length))) < 1.0e-4) {
        return CameraStatus::up_collinear;
    }
    return CameraStatus::valid;
}

CameraParse parse_camera(const std::string_view text, const std::uint32_t width, const std::uint32_t height) {
    CameraParse result;
    std::array<float, 12> values{};
    std::size_t count = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(',', start), text.size());
        const std::string token(text.substr(start, end - start));
        if (count == values.size() || token.empty()) return result;
        char* stop = nullptr;
        const float value = std::strtof(token.c_str(), &stop);
        if (stop == token.c_str() || *stop != '\0') return result;
        values[count++] = value;
        start = end + 1;
    }
    if (count != values.size()) return result;
    FixedCamera& camera = result.camera;
    camera.width = width;
    camera.height = height;
    camera.eye = {values[0], values[1], values[2]};
    camera.target = {values[3], values[4], values[5]};
    camera.up = {values[6], values[7], values[8]};
    camera.vertical_fov_degrees = values[9];
    camera.near_plane = values[10];
    camera.far_plane = values[11];
    result.status = validate_camera(camera);
    return result;
}

// -- mode-7 sun reference geometry ------------------------------------------------

namespace {

[[nodiscard]] V3 plus(const V3& a, const V3& b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] SunVec3 to_array(const V3& value) noexcept { return {value.x, value.y, value.z}; }
[[nodiscard]] V3 from(const SunVec3& value) noexcept { return {value[0], value[1], value[2]}; }

// One bind record applied to a point, in double.
[[nodiscard]] V3 bind_point(const Rigid& m, const V3& p) noexcept {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
            m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

[[nodiscard]] std::string bone_label(const std::span<const assets::Bone> bones, const std::int32_t index) {
    return "bone " + std::to_string(index) + " '" + bones[static_cast<std::size_t>(index)].name + "'";
}

[[nodiscard]] SunReferencePlacement sun_failure(const SunPlacementStatus status, std::string detail) {
    SunReferencePlacement result;
    result.status = status;
    result.detail = std::move(detail);
    return result;
}

} // namespace

std::string_view to_string(const SunPlacementStatus status) noexcept {
    switch (status) {
    case SunPlacementStatus::placed: return "placed";
    case SunPlacementStatus::mode_not_sun: return "mode_not_sun";
    case SunPlacementStatus::chain_invalid: return "chain_invalid";
    case SunPlacementStatus::chain_billboard_ancestor: return "chain_billboard_ancestor";
    case SunPlacementStatus::chain_not_proper_rigid: return "chain_not_proper_rigid";
    case SunPlacementStatus::camera_nonfinite: return "camera_nonfinite";
    case SunPlacementStatus::camera_direction_degenerate: return "camera_direction_degenerate";
    case SunPlacementStatus::camera_up_collinear: return "camera_up_collinear";
    case SunPlacementStatus::sun_direction_nonfinite: return "sun_direction_nonfinite";
    case SunPlacementStatus::sun_direction_zero: return "sun_direction_zero";
    case SunPlacementStatus::sun_direction_vertical: return "sun_direction_vertical";
    }
    return "invalid";
}

SunReferencePlacement sun_reference_placement(const SunReferenceInput& input) {
    using Status = SunPlacementStatus;
    const std::span<const assets::Bone> bones = input.bones;
    const auto in_range = [&bones](const std::int32_t index) {
        return index >= 0 && static_cast<std::size_t>(index) < bones.size();
    };

    // Mode: exactly 7 on the mesh's own bone (its index must be readable).
    if (!in_range(input.mesh_bone)) {
        return sun_failure(Status::chain_invalid, "mesh bone index " + std::to_string(input.mesh_bone)
            + " is out of range for " + std::to_string(bones.size()) + " bone(s)");
    }
    const assets::Bone& own = bones[static_cast<std::size_t>(input.mesh_bone)];
    if (own.billboard != sun_billboard_mode) {
        return sun_failure(Status::mode_not_sun, bone_label(bones, input.mesh_bone) + " has billboard mode "
            + std::to_string(own.billboard) + "; only mode 7 (sun) is evaluated");
    }

    // Chain structure, then ancestor modes, then rigidity, each over the whole chain.
    std::vector<std::int32_t> chain;
    for (std::int32_t current = input.mesh_bone; current >= 0;) {
        if (chain.size() >= bones.size()) {
            return sun_failure(Status::chain_invalid, "the bone chain from " + bone_label(bones, input.mesh_bone)
                + " does not terminate");
        }
        chain.push_back(current);
        const std::int32_t parent = bones[static_cast<std::size_t>(current)].parent;
        if (parent < -1 || (parent >= 0 && !in_range(parent))) {
            return sun_failure(Status::chain_invalid, bone_label(bones, current) + " has invalid parent index "
                + std::to_string(parent));
        }
        current = parent;
    }
    for (std::size_t link = 1; link < chain.size(); ++link) {
        const assets::Bone& ancestor = bones[static_cast<std::size_t>(chain[link])];
        if (ancestor.billboard != 0) {
            return sun_failure(Status::chain_billboard_ancestor, "ancestor " + bone_label(bones, chain[link])
                + " has billboard mode " + std::to_string(ancestor.billboard));
        }
    }
    for (const std::int32_t index : chain) {
        if (!proper_rigid(bones[static_cast<std::size_t>(index)].relative_transform)) {
            return sun_failure(Status::chain_not_proper_rigid, bone_label(bones, index)
                + " has a non-finite, scaled, sheared, or reflected relative transform");
        }
    }

    // Camera: validate_camera's thresholds, in the source basis.
    const std::array<std::pair<std::string_view, const assets::Vec3f*>, 3> camera_fields{{
        {"eye", &input.eye}, {"target", &input.target}, {"up", &input.up}}};
    for (const auto& [name, value] : camera_fields) {
        if (!finite(*value)) {
            return sun_failure(Status::camera_nonfinite, "camera " + std::string(name) + " is not finite");
        }
    }
    const V3 forward = sub(from(input.target), from(input.eye));
    const double distance = length(forward);
    if (!(distance > sun_camera_min_distance)) {
        return sun_failure(Status::camera_direction_degenerate, "camera eye and target coincide");
    }
    const V3 up = from(input.up);
    const double up_length = length(up);
    if (!(up_length > sun_camera_min_up_length)) {
        return sun_failure(Status::camera_up_collinear, "camera up is zero");
    }
    const V3 unit_forward = scale(forward, 1.0 / distance);
    const V3 side = cross(unit_forward, scale(up, 1.0 / up_length));
    const double side_length = length(side);
    if (side_length < sun_camera_min_up_sine) {
        return sun_failure(Status::camera_up_collinear, "camera up is collinear with the view direction");
    }

    // L: finite, not zero, not (near) vertical.
    if (!finite(input.toward_sun)) {
        return sun_failure(Status::sun_direction_nonfinite, "toward_sun is not finite");
    }
    const V3 toward = from(input.toward_sun);
    if (toward.x == 0.0 && toward.y == 0.0 && toward.z == 0.0) {
        return sun_failure(Status::sun_direction_zero, "toward_sun is zero");
    }
    const double horizontal = std::hypot(toward.x, toward.y);
    const double magnitude = std::hypot(horizontal, toward.z);
    if (!(horizontal > sun_direction_min_horizontal_ratio * magnitude)) {
        return sun_failure(Status::sun_direction_vertical, "toward_sun is vertical or near vertical");
    }

    SunReferencePlacement result;
    result.status = Status::placed;
    const V3 right = scale(side, 1.0 / side_length);
    result.right = to_array(right);
    result.view_up = to_array(cross(right, unit_forward));
    result.backward = to_array(scale(unit_forward, -1.0));

    result.tilt = std::atan2(toward.z, horizontal);
    result.azimuth = std::atan2(toward.y, toward.x);
    const double ct = std::cos(result.tilt);
    const double st = std::sin(result.tilt);
    const double ca = std::cos(result.azimuth);
    const double sa = std::sin(result.azimuth);
    const V3 sun_x{ct * ca, ct * sa, st};
    const V3 sun_y{-sa, ca, 0.0};
    const V3 sun_z{-st * ca, -st * sa, ct};
    result.sun_axes = {to_array(sun_x), to_array(sun_y), to_array(sun_z)};

    // o: the mesh bone's origin through its own record, then each parent's.
    V3 origin{};
    for (const std::int32_t index : chain) {
        origin = bind_point(bones[static_cast<std::size_t>(index)].relative_transform, origin);
    }
    result.rest_origin = to_array(origin);
    result.sun_origin = to_array(plus(plus(scale(sun_x, origin.x), scale(sun_y, origin.y)), scale(sun_z, origin.z)));
    return result;
}

std::optional<SunVec3> sun_reference_vertex(const SunReferencePlacement& placement, const assets::Vec3f& local) noexcept {
    if (placement.status != SunPlacementStatus::placed || !finite(local)) return std::nullopt;
    const V3 placed = plus(plus(scale(from(placement.right), local.x), scale(from(placement.view_up), local.z)),
                           plus(scale(from(placement.backward), -static_cast<double>(local.y)), from(placement.sun_origin)));
    return to_array(placed);
}

assets::Vec3f source_from_render(const std::array<float, 3>& render) noexcept {
    return {render[0], -render[2], render[1]};
}

// -- regions --------------------------------------------------------------------

std::vector<Triangle> render_triangles(const assets::Submesh& submesh) {
    std::vector<Triangle> result;
    result.reserve(submesh.indices.size() / 3);
    for (std::size_t index = 0; index + 2 < submesh.indices.size(); index += 3) {
        Triangle triangle{};
        bool valid = true;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const std::size_t vertex = submesh.indices[index + corner];
            if (vertex >= submesh.vertices.size()) {
                valid = false;
                break;
            }
            triangle[corner] = terrain::asset_to_render(submesh.vertices[vertex].position);
        }
        if (valid) result.push_back(triangle);
    }
    return result;
}

std::array<std::vector<Triangle>, 4> render_triangles_by_uv_quadrant(const assets::Submesh& submesh) {
    std::array<std::vector<Triangle>, 4> result;
    for (std::size_t index = 0; index + 2 < submesh.indices.size(); index += 3) {
        Triangle triangle{};
        float u = 0.0F;
        float v = 0.0F;
        bool valid = true;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const std::size_t vertex = submesh.indices[index + corner];
            if (vertex >= submesh.vertices.size()) {
                valid = false;
                break;
            }
            triangle[corner] = terrain::asset_to_render(submesh.vertices[vertex].position);
            u += submesh.vertices[vertex].texcoord[0].x / 3.0F;
            v += submesh.vertices[vertex].texcoord[0].y / 3.0F;
        }
        if (!valid) continue;
        const std::size_t quadrant = (u >= 0.5F ? 1U : 0U) + (v >= 0.5F ? 2U : 0U);
        result[quadrant].push_back(triangle);
    }
    return result;
}

std::uint64_t ScreenMask::count() const noexcept {
    return static_cast<std::uint64_t>(std::count(bits.begin(), bits.end(), std::uint8_t{1}));
}

bool ScreenMask::at(const std::uint32_t x, const std::uint32_t y) const noexcept {
    if (x >= width || y >= height) return false;
    return bits[static_cast<std::size_t>(y) * width + x] != 0U;
}

namespace {

struct CameraSpace final {
    V3 eye;
    V3 right;
    V3 up;
    V3 forward;
    double tan_half{};
    double aspect{};
};

[[nodiscard]] CameraSpace camera_space(const FixedCamera& camera) {
    // Godot's Transform3D::looking_at: z = -(target - eye), x = up x z,
    // y = z x x. Screen right is x, screen up is y, view depth is along -z.
    CameraSpace space;
    space.eye = from(camera.eye);
    const V3 forward = sub(from(camera.target), space.eye);
    space.forward = scale(forward, 1.0 / length(forward));
    const V3 back = scale(space.forward, -1.0);
    const V3 right = cross(from(camera.up), back);
    space.right = scale(right, 1.0 / length(right));
    space.up = cross(back, space.right);
    space.tan_half = std::tan(static_cast<double>(camera.vertical_fov_degrees) * 0.5 * 3.14159265358979323846 / 180.0);
    space.aspect = static_cast<double>(camera.width) / static_cast<double>(camera.height);
    return space;
}

struct ViewPoint final {
    double x{}, y{}, depth{};
};

[[nodiscard]] std::vector<ViewPoint> clip(std::vector<ViewPoint> polygon, const double plane, const bool keep_greater) {
    std::vector<ViewPoint> output;
    if (polygon.empty()) return output;
    const auto inside = [&](const ViewPoint& point) {
        return keep_greater ? point.depth >= plane : point.depth <= plane;
    };
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const ViewPoint& current = polygon[index];
        const ViewPoint& next = polygon[(index + 1) % polygon.size()];
        const bool current_in = inside(current);
        const bool next_in = inside(next);
        if (current_in) output.push_back(current);
        if (current_in != next_in) {
            const double t = (plane - current.depth) / (next.depth - current.depth);
            output.push_back({current.x + (next.x - current.x) * t, current.y + (next.y - current.y) * t, plane});
        }
    }
    return output;
}

} // namespace

ScreenMask rasterize(const FixedCamera& camera, const std::span<const Triangle> triangles) {
    ScreenMask mask;
    mask.width = camera.width;
    mask.height = camera.height;
    mask.bits.assign(static_cast<std::size_t>(camera.width) * camera.height, 0U);
    if (validate_camera(camera) != CameraStatus::valid) return mask;
    const CameraSpace space = camera_space(camera);
    const double width = static_cast<double>(camera.width);
    const double height = static_cast<double>(camera.height);
    for (const Triangle& triangle : triangles) {
        std::vector<ViewPoint> polygon;
        for (const assets::Vec3f& corner : triangle) {
            const V3 relative = sub(from(corner), space.eye);
            polygon.push_back({dot(relative, space.right), dot(relative, space.up), dot(relative, space.forward)});
        }
        polygon = clip(std::move(polygon), camera.near_plane, true);
        polygon = clip(std::move(polygon), camera.far_plane, false);
        if (polygon.size() < 3) continue;
        std::vector<std::array<double, 2>> screen;
        for (const ViewPoint& point : polygon) {
            const double ndc_x = (point.x / point.depth) / (space.tan_half * space.aspect);
            const double ndc_y = (point.y / point.depth) / space.tan_half;
            screen.push_back({(ndc_x + 1.0) * 0.5 * width, (1.0 - ndc_y) * 0.5 * height});
        }
        double area = 0.0;
        double left = std::numeric_limits<double>::max();
        double right = std::numeric_limits<double>::lowest();
        double top = std::numeric_limits<double>::max();
        double bottom = std::numeric_limits<double>::lowest();
        for (std::size_t index = 0; index < screen.size(); ++index) {
            const auto& a = screen[index];
            const auto& b = screen[(index + 1) % screen.size()];
            area += a[0] * b[1] - b[0] * a[1];
            left = std::min(left, a[0]);
            right = std::max(right, a[0]);
            top = std::min(top, a[1]);
            bottom = std::max(bottom, a[1]);
        }
        if (std::abs(area) < 1.0e-9) continue;
        const double orientation = area > 0.0 ? 1.0 : -1.0;
        const auto x0 = static_cast<std::int64_t>(std::max(0.0, std::floor(left)));
        const auto x1 = static_cast<std::int64_t>(std::min(width - 1.0, std::ceil(right)));
        const auto y0 = static_cast<std::int64_t>(std::max(0.0, std::floor(top)));
        const auto y1 = static_cast<std::int64_t>(std::min(height - 1.0, std::ceil(bottom)));
        for (std::int64_t y = y0; y <= y1; ++y) {
            for (std::int64_t x = x0; x <= x1; ++x) {
                const double px = static_cast<double>(x) + 0.5;
                const double py = static_cast<double>(y) + 0.5;
                bool inside = true;
                for (std::size_t index = 0; index < screen.size() && inside; ++index) {
                    const auto& a = screen[index];
                    const auto& b = screen[(index + 1) % screen.size()];
                    const double edge = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
                    if (edge * orientation < 0.0) inside = false;
                }
                if (inside) {
                    mask.bits[static_cast<std::size_t>(y) * camera.width + static_cast<std::size_t>(x)] = 1U;
                }
            }
        }
    }
    return mask;
}

namespace {

// A square structuring element is separable: one pass along rows, then one
// along columns, gives exactly the (2r+1)^2 result. Outside the frame counts
// as uncovered, so erosion also shrinks a region away from the frame edge.
[[nodiscard]] ScreenMask morph(const ScreenMask& mask, const std::uint32_t radius, const bool grow) {
    if (radius == 0) return mask;
    const auto r = static_cast<std::int64_t>(radius);
    const auto w = static_cast<std::int64_t>(mask.width);
    const auto h = static_cast<std::int64_t>(mask.height);
    const auto pass = [&](const std::vector<std::uint8_t>& input, const bool along_rows) {
        std::vector<std::uint8_t> output(input.size(), 0U);
        const std::int64_t lines = along_rows ? h : w;
        const std::int64_t span = along_rows ? w : h;
        for (std::int64_t line = 0; line < lines; ++line) {
            const auto index = [&](const std::int64_t position) {
                return static_cast<std::size_t>(along_rows ? line * w + position : position * w + line);
            };
            // Prefix counts of set pixels along the line.
            std::vector<std::int64_t> prefix(static_cast<std::size_t>(span) + 1U, 0);
            for (std::int64_t position = 0; position < span; ++position) {
                prefix[static_cast<std::size_t>(position) + 1U] =
                    prefix[static_cast<std::size_t>(position)] + (input[index(position)] != 0U ? 1 : 0);
            }
            for (std::int64_t position = 0; position < span; ++position) {
                const std::int64_t low = position - r;
                const std::int64_t high = position + r;
                const std::int64_t set = prefix[static_cast<std::size_t>(std::min(high, span - 1)) + 1U]
                    - prefix[static_cast<std::size_t>(std::max(low, std::int64_t{0}))];
                const bool value = grow ? set > 0 : (low >= 0 && high < span && set == 2 * r + 1);
                output[index(position)] = value ? 1U : 0U;
            }
        }
        return output;
    };
    ScreenMask result = mask;
    result.bits = pass(pass(mask.bits, true), false);
    return result;
}

[[nodiscard]] bool changed(const Rgb8Image& left, const Rgb8Image& right, const std::size_t pixel) noexcept {
    const std::size_t offset = pixel * 3;
    std::uint32_t sum = 0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const int difference = static_cast<int>(left.rgb[offset + channel]) - static_cast<int>(right.rgb[offset + channel]);
        sum += static_cast<std::uint32_t>(difference < 0 ? -difference : difference);
    }
    return sum > changed_threshold;
}

[[nodiscard]] bool same_shape(const Rgb8Image& image, const std::uint32_t width, const std::uint32_t height) noexcept {
    return image.width == width && image.height == height
        && image.rgb.size() == static_cast<std::size_t>(width) * height * 3U;
}

} // namespace

ScreenMask erode(const ScreenMask& mask, const std::uint32_t radius) { return morph(mask, radius, false); }
ScreenMask dilate(const ScreenMask& mask, const std::uint32_t radius) { return morph(mask, radius, true); }

ScreenMask unite(const ScreenMask& left, const ScreenMask& right) {
    ScreenMask result = left;
    for (std::size_t index = 0; index < result.bits.size() && index < right.bits.size(); ++index) {
        if (right.bits[index] != 0U) result.bits[index] = 1U;
    }
    return result;
}

std::string_view to_string(const PixelStatus status) noexcept {
    switch (status) {
    case PixelStatus::verified: return "verified";
    case PixelStatus::not_projected: return "not_projected";
    case PixelStatus::occluded: return "occluded";
    case PixelStatus::no_change: return "no_change";
    case PixelStatus::inconclusive: return "inconclusive";
    case PixelStatus::leaked_outside_region: return "leaked_outside_region";
    case PixelStatus::not_submitted: return "not_submitted";
    }
    return "invalid";
}

PixelEvaluation evaluate_pixels(
    const std::span<const SurfaceRegions> regions,
    const Rgb8Image& configured,
    const Rgb8Image& disabled,
    const std::span<const std::optional<Rgb8Image>> isolated,
    const std::optional<ScreenMask>& occluder) {
    PixelEvaluation result;
    if (regions.empty()) {
        result.status = "failed";
        result.failure = "no surface region was declared";
        return result;
    }
    const std::uint32_t width = regions.front().mask.width;
    const std::uint32_t height = regions.front().mask.height;
    if (!same_shape(configured, width, height) || !same_shape(disabled, width, height)
        || isolated.size() != regions.size()) {
        result.status = "failed";
        result.failure = "capture dimensions disagree with the camera viewport or the declared regions";
        return result;
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    for (const SurfaceRegions& region : regions) {
        bool shaped = region.mask.width == width && region.mask.height == height && region.mask.bits.size() == pixels;
        for (const ScreenMask& quadrant : region.quadrants) {
            shaped = shaped && (quadrant.bits.empty() || quadrant.bits.size() == pixels);
        }
        if (!shaped) {
            result.status = "failed";
            result.failure = "a declared region disagrees with the camera viewport";
            return result;
        }
    }
    if (occluder && occluder->bits.size() != pixels) {
        result.status = "failed";
        result.failure = "the occluder region disagrees with the camera viewport";
        return result;
    }
    std::vector<ScreenMask> grown;
    ScreenMask all_grown;
    all_grown.width = width;
    all_grown.height = height;
    all_grown.bits.assign(pixels, 0U);
    for (const SurfaceRegions& region : regions) {
        grown.push_back(dilate(region.mask, mask_margin));
        all_grown = unite(all_grown, grown.back());
    }
    std::optional<ScreenMask> occluder_grown;
    if (occluder) occluder_grown = dilate(*occluder, mask_margin);

    bool failed = false;
    bool weak = false;
    for (std::size_t surface = 0; surface < regions.size(); ++surface) {
        const SurfaceRegions& region = regions[surface];
        SurfacePixels evidence;
        evidence.mask_pixels = region.mask.count();
        ScreenMask interior = erode(region.mask, mask_margin);
        const bool projected = interior.count() != 0;
        for (std::size_t other = 0; other < regions.size(); ++other) {
            if (other == surface) continue;
            for (std::size_t index = 0; index < pixels; ++index) {
                if (grown[other].bits[index] != 0U) interior.bits[index] = 0U;
            }
        }
        if (occluder_grown) {
            for (std::size_t index = 0; index < pixels; ++index) {
                if (occluder_grown->bits[index] != 0U) interior.bits[index] = 0U;
            }
        }
        evidence.interior_pixels = interior.count();
        for (std::size_t index = 0; index < pixels; ++index) {
            if (interior.bits[index] != 0U && changed(configured, disabled, index)) ++evidence.changed_interior;
        }
        for (std::size_t quadrant = 0; quadrant < 4; ++quadrant) {
            if (region.quadrants[quadrant].bits.empty()) continue;
            const ScreenMask inner = erode(region.quadrants[quadrant], mask_margin);
            std::array<double, 3> sum{};
            std::uint64_t count = 0;
            for (std::size_t index = 0; index < pixels; ++index) {
                if (inner.bits[index] == 0U || interior.bits[index] == 0U) continue;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    sum[channel] += configured.rgb[index * 3 + channel];
                }
                ++count;
            }
            evidence.quadrant_pixels[quadrant] = count;
            if (count != 0) {
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    evidence.quadrant_mean_rgb[quadrant][channel] = sum[channel] / static_cast<double>(count);
                }
            }
        }
        const std::optional<Rgb8Image>& alone = isolated[surface];
        if (alone && same_shape(*alone, width, height)) {
            for (std::size_t index = 0; index < pixels; ++index) {
                const bool inside = grown[surface].bits[index] != 0U;
                if (!inside) ++evidence.isolated_pixels_outside;
                if (!changed(*alone, disabled, index)) continue;
                if (inside) ++evidence.isolated_changed_inside;
                else ++evidence.isolated_changed_outside;
            }
        }
        if (evidence.interior_pixels == 0) {
            evidence.status = projected ? PixelStatus::occluded : PixelStatus::not_projected;
        } else if (!alone) {
            evidence.status = PixelStatus::not_submitted;
        } else if (evidence.isolated_changed_outside * 500 > evidence.isolated_pixels_outside) {
            evidence.status = PixelStatus::leaked_outside_region;
        } else if (evidence.changed_interior == 0 || evidence.isolated_changed_inside == 0) {
            evidence.status = PixelStatus::no_change;
        } else if (evidence.changed_interior * 10 < evidence.interior_pixels * 9) {
            evidence.status = PixelStatus::inconclusive;
        } else {
            evidence.status = PixelStatus::verified;
        }
        if (evidence.status == PixelStatus::inconclusive) {
            weak = true;
        } else if (evidence.status != PixelStatus::verified) {
            failed = true;
            if (result.failure.empty()) {
                result.failure = "surface " + std::to_string(surface) + " pixel evidence: "
                    + std::string(to_string(evidence.status));
            }
        }
        result.surfaces.push_back(evidence);
    }
    for (std::size_t index = 0; index < pixels; ++index) {
        if (all_grown.bits[index] != 0U) continue;
        ++result.outside_pixels;
        if (changed(configured, disabled, index)) ++result.changed_outside;
    }
    if (result.changed_outside * 500 > result.outside_pixels) {
        failed = true;
        if (result.failure.empty()) result.failure = "the sky changed pixels outside every surface's predeclared region";
    }
    if (occluder) {
        const ScreenMask inner = erode(*occluder, mask_margin);
        for (std::size_t index = 0; index < pixels; ++index) {
            if (inner.bits[index] == 0U) continue;
            ++result.occluder_pixels;
            if (changed(configured, disabled, index)) ++result.occluder_changed;
        }
        if (result.occluder_pixels == 0) {
            result.occlusion_status = "not_projected";
            failed = true;
            if (result.failure.empty()) result.failure = "the foreground occluder control does not project";
        } else if (result.occluder_changed * 500 > result.occluder_pixels) {
            result.occlusion_status = "failed";
            failed = true;
            if (result.failure.empty()) result.failure = "the sky drew over the foreground occluder";
        } else {
            result.occlusion_status = "verified";
        }
    }
    result.status = failed ? "failed" : (weak ? "inconclusive" : "verified");
    return result;
}

} // namespace eawr::presentation::space
