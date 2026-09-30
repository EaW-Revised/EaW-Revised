#include "eawr/presentation/space/sun_retail.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace eawr::presentation::space {
namespace {

struct V3 final {
    double x{};
    double y{};
    double z{};
};

[[nodiscard]] V3 from(const assets::Vec3f& value) noexcept { return {value.x, value.y, value.z}; }
[[nodiscard]] V3 from(const SunVec3& value) noexcept { return {value[0], value[1], value[2]}; }
[[nodiscard]] SunVec3 to_array(const V3& value) noexcept { return {value.x, value.y, value.z}; }
[[nodiscard]] V3 plus(const V3& a, const V3& b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] V3 sub(const V3& a, const V3& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] V3 scale(const V3& a, const double s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] V3 cross(const V3& a, const V3& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] double length(const V3& a) noexcept { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

[[nodiscard]] bool finite(const assets::Vec3f& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

// The planner's proper-rigid test (space.cpp), repeated verbatim so this
// policy's chain facts cannot drift from the planner's verdicts; a contract
// cross-checks the two on the same records. Its per-bone tolerance bounds each
// link, not the chain: s is measured separately.
[[nodiscard]] bool proper_rigid(const std::array<float, 12>& m) noexcept {
    if (!std::all_of(m.begin(), m.end(), [](const float v) { return std::isfinite(v); })) return false;
    const assets::Vec3f axes[3]{{m[0], m[4], m[8]}, {m[1], m[5], m[9]}, {m[2], m[6], m[10]}};
    const auto dot = [](const assets::Vec3f a, const assets::Vec3f b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    constexpr float tolerance = 1.0e-4F;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(dot(axes[i], axes[i]) - 1.0F) > tolerance) return false;
        for (int j = 0; j < i; ++j) if (std::abs(dot(axes[i], axes[j])) > tolerance) return false;
    }
    const auto& a = axes[0]; const auto& b = axes[1]; const auto& c = axes[2];
    const float determinant = a.x * (b.y * c.z - b.z * c.y)
        + a.y * (b.z * c.x - b.x * c.z) + a.z * (b.x * c.y - b.y * c.x);
    return std::abs(determinant - 1.0F) <= tolerance;
}

// A record's rotation applied to a direction (translation ignored), in double.
[[nodiscard]] V3 rotate(const std::array<float, 12>& m, const V3& v) noexcept {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z,
            m[4] * v.x + m[5] * v.y + m[6] * v.z,
            m[8] * v.x + m[9] * v.y + m[10] * v.z};
}

[[nodiscard]] std::string bone_label(const std::span<const assets::Bone> bones, const std::int32_t index) {
    return "bone " + std::to_string(index) + " '" + bones[static_cast<std::size_t>(index)].name + "'";
}

[[nodiscard]] SunRetailPlacement retail_failure(const SunRetailStatus status, std::string detail) {
    SunRetailPlacement result;
    result.status = status;
    result.detail = std::move(detail);
    return result;
}

// Gate rows. Closing one is a reviewed change that removes its row here and
// updates docs/rendering.md#sun-policies.
constexpr std::array<SunRetailGate, 8> open_gates{{
    {"G-01a", "the final deferred-draw WORLD hop from the billboarded bone matrix, including an attached "
              "object's own local offset (R-M7-10)"},
    {"G-02", "far plane, depth state and draw order against the star sphere for a camera-centred origin at "
             "distance d"},
    {"G-03", "how TED/map environment data supply the light-0 angles (units, sign, environment selection); "
             "L is a caller input"},
    {"G-06", "ambient cull, depth-enable, fog, blend, alpha-write and sRGB states of the sun draw"},
    {"G-07", "animated mode-7 bones: no animation input; rejected before admission"},
    {"G-08", "Remake-profile sun billboards were not scanned for authored values or chains"},
    {"G-12", "the size factor s under a non-unit sky-object world scale: no world input"},
    {"WP16-PASS", "which render passes run the rule; retail skips the shadow camera and keeps the last "
                  "transform (R-M7-08), which a stateless policy cannot reproduce"},
}};

} // namespace

std::string_view to_string(const SunRetailStatus status) noexcept {
    switch (status) {
    case SunRetailStatus::placed: return "placed";
    case SunRetailStatus::mode_not_sun: return "mode_not_sun";
    case SunRetailStatus::mode_sun_alias: return "mode_sun_alias";
    case SunRetailStatus::chain_invalid: return "chain_invalid";
    case SunRetailStatus::chain_billboard_ancestor: return "chain_billboard_ancestor";
    case SunRetailStatus::chain_not_proper_rigid: return "chain_not_proper_rigid";
    case SunRetailStatus::sun_distance_zero: return "sun_distance_zero";
    case SunRetailStatus::camera_nonfinite: return "camera_nonfinite";
    case SunRetailStatus::camera_direction_degenerate: return "camera_direction_degenerate";
    case SunRetailStatus::camera_up_collinear: return "camera_up_collinear";
    case SunRetailStatus::sun_direction_nonfinite: return "sun_direction_nonfinite";
    case SunRetailStatus::sun_direction_zero: return "sun_direction_zero";
    case SunRetailStatus::sun_direction_not_unit: return "sun_direction_not_unit";
    case SunRetailStatus::sun_along_camera_up: return "sun_along_camera_up";
    }
    return "invalid";
}

SunRetailPlacement sun_retail_placement(const SunRetailInput& input) {
    using Status = SunRetailStatus;
    const std::span<const assets::Bone> bones = input.bones;
    const auto in_range = [&bones](const std::int32_t index) {
        return index >= 0 && static_cast<std::size_t>(index) < bones.size();
    };

    // Mode: exactly 7 on the mesh's own bone (its index must be readable).
    if (!in_range(input.mesh_bone)) {
        return retail_failure(Status::chain_invalid, "mesh bone index " + std::to_string(input.mesh_bone)
            + " is out of range for " + std::to_string(bones.size()) + " bone(s)");
    }
    const assets::Bone& own = bones[static_cast<std::size_t>(input.mesh_bone)];
    if (sun_retail_draw_mode(own.billboard) != sun_billboard_mode) {
        return retail_failure(Status::mode_not_sun, bone_label(bones, input.mesh_bone) + " has billboard mode "
            + std::to_string(own.billboard) + "; only mode 7 (sun) is evaluated");
    }
    if (own.billboard != sun_billboard_mode) {
        return retail_failure(Status::mode_sun_alias, bone_label(bones, input.mesh_bone) + " has authored mode "
            + std::to_string(own.billboard) + ", drawn as mode 7 but outside this policy");
    }

    // Chain structure, then ancestor modes, then rigidity, each over the whole chain.
    std::vector<std::int32_t> chain;
    for (std::int32_t current = input.mesh_bone; current >= 0;) {
        if (chain.size() >= bones.size()) {
            return retail_failure(Status::chain_invalid, "the bone chain from " + bone_label(bones, input.mesh_bone)
                + " does not terminate");
        }
        chain.push_back(current);
        const std::int32_t parent = bones[static_cast<std::size_t>(current)].parent;
        if (parent < -1 || (parent >= 0 && !in_range(parent))) {
            return retail_failure(Status::chain_invalid, bone_label(bones, current) + " has invalid parent index "
                + std::to_string(parent));
        }
        current = parent;
    }
    for (std::size_t link = 1; link < chain.size(); ++link) {
        const assets::Bone& ancestor = bones[static_cast<std::size_t>(chain[link])];
        if (ancestor.billboard != 0) {
            return retail_failure(Status::chain_billboard_ancestor, "ancestor " + bone_label(bones, chain[link])
                + " has billboard mode " + std::to_string(ancestor.billboard));
        }
    }
    for (const std::int32_t index : chain) {
        if (!proper_rigid(bones[static_cast<std::size_t>(index)].relative_transform)) {
            return retail_failure(Status::chain_not_proper_rigid, bone_label(bones, index)
                + " has a non-finite, scaled, sheared, or reflected relative transform");
        }
    }

    // s: the third rotation column of the stored matrix, the identity world
    // times every record below the root (chain.back()), mesh bone first.
    V3 column{0.0, 0.0, 1.0};
    for (std::size_t link = 0; link + 1 < chain.size(); ++link) {
        column = rotate(bones[static_cast<std::size_t>(chain[link])].relative_transform, column);
    }
    const double size = length(column);

    // d: the length of the own authored translation; its direction is not used.
    const auto& record = own.relative_transform;
    const double distance = length({record[3], record[7], record[11]});
    if (!(distance > sun_retail_min_distance)) {
        return retail_failure(Status::sun_distance_zero, bone_label(bones, input.mesh_bone)
            + " has a zero own translation; the retail rule collapses the mesh");
    }

    // Camera: validate_camera's thresholds, in the source basis.
    const std::array<std::pair<std::string_view, const assets::Vec3f*>, 3> camera_fields{{
        {"eye", &input.eye}, {"target", &input.target}, {"up", &input.up}}};
    for (const auto& [name, value] : camera_fields) {
        if (!finite(*value)) {
            return retail_failure(Status::camera_nonfinite, "camera " + std::string(name) + " is not finite");
        }
    }
    const V3 forward = sub(from(input.target), from(input.eye));
    const double forward_length = length(forward);
    if (!(forward_length > sun_camera_min_distance)) {
        return retail_failure(Status::camera_direction_degenerate, "camera eye and target coincide");
    }
    const V3 up = from(input.up);
    const double up_length = length(up);
    if (!(up_length > sun_camera_min_up_length)) {
        return retail_failure(Status::camera_up_collinear, "camera up is zero");
    }
    const V3 unit_forward = scale(forward, 1.0 / forward_length);
    const V3 side = cross(unit_forward, scale(up, 1.0 / up_length));
    const double side_length = length(side);
    if (side_length < sun_camera_min_up_sine) {
        return retail_failure(Status::camera_up_collinear, "camera up is collinear with the view direction");
    }
    const V3 right = scale(side, 1.0 / side_length);
    const V3 view_up = cross(right, unit_forward);

    // L: finite, not zero, unit, not along the camera up axis.
    if (!finite(input.toward_light)) {
        return retail_failure(Status::sun_direction_nonfinite, "toward_light is not finite");
    }
    const V3 toward = from(input.toward_light);
    if (toward.x == 0.0 && toward.y == 0.0 && toward.z == 0.0) {
        return retail_failure(Status::sun_direction_zero, "toward_light is zero");
    }
    const double magnitude = length(toward);
    if (!(std::abs(magnitude - 1.0) <= sun_retail_unit_tolerance)) {
        return retail_failure(Status::sun_direction_not_unit, "toward_light has length " + std::to_string(magnitude)
            + "; a unit vector is required");
    }
    const V3 normal = scale(toward, -1.0 / magnitude);
    const V3 roll = cross(view_up, normal);
    const double roll_length = length(roll);
    if (roll_length < sun_retail_min_up_sine) {
        return retail_failure(Status::sun_along_camera_up, "toward_light is parallel to the camera up axis");
    }

    SunRetailPlacement result;
    result.status = Status::placed;
    result.distance = distance;
    result.scale = size;
    result.origin = to_array(plus(from(input.eye), scale(toward, distance)));
    result.right = to_array(right);
    result.view_up = to_array(view_up);
    result.backward = to_array(scale(unit_forward, -1.0));
    const V3 axis_x = scale(roll, 1.0 / roll_length);
    result.axes = {to_array(axis_x), to_array(normal), to_array(cross(normal, axis_x))};
    return result;
}

std::optional<SunVec3> sun_retail_vertex(const SunRetailPlacement& placement, const assets::Vec3f& local) noexcept {
    if (placement.status != SunRetailStatus::placed || !finite(local)) return std::nullopt;
    const V3 offset = plus(plus(scale(from(placement.axes[0]), local.x), scale(from(placement.axes[1]), local.y)),
                           scale(from(placement.axes[2]), local.z));
    return to_array(plus(from(placement.origin), scale(offset, placement.scale)));
}

std::span<const SunRetailGate> sun_retail_open_gates() noexcept { return open_gates; }

bool sun_retail_admissible(const SunRetailPlacement& placement) noexcept {
    return placement.status == SunRetailStatus::placed && sun_retail_open_gates().empty();
}

} // namespace eawr::presentation::space
