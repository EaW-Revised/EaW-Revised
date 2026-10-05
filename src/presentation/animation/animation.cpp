#include "eawr/presentation/animation/animation.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>

namespace eawr::presentation::animation {
namespace {

using Vec3 = assets::Vec3f;
using Quat = assets::Vec4f;

[[nodiscard]] core::Diagnostic failure(
    const std::string_view code, std::string message, const std::string_view path = {}) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = core::Severity::error;
    diagnostic.message = std::move(message);
    if (!path.empty()) diagnostic.logical_path = std::string(path);
    return diagnostic;
}

[[nodiscard]] bool finite(const float value) noexcept { return std::isfinite(value); }
[[nodiscard]] bool finite(const Vec3 value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z);
}
[[nodiscard]] bool finite(const Quat value) noexcept {
    return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
}
[[nodiscard]] bool finite(const Matrix& matrix) noexcept {
    return std::all_of(matrix.begin(), matrix.end(), [](const float value) { return finite(value); });
}
[[nodiscard]] float dot(const Quat left, const Quat right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z + left.w * right.w;
}
[[nodiscard]] Quat normalized(Quat value) noexcept {
    const float length_squared = dot(value, value);
    if (!finite(length_squared) || length_squared <= std::numeric_limits<float>::epsilon()) {
        return {0.0F, 0.0F, 0.0F, 1.0F};
    }
    const float inverse_length = 1.0F / std::sqrt(length_squared);
    value.x *= inverse_length; value.y *= inverse_length; value.z *= inverse_length; value.w *= inverse_length;
    return value;
}
[[nodiscard]] Vec3 lerp(const Vec3 left, const Vec3 right, const float amount) noexcept {
    return {left.x + (right.x - left.x) * amount, left.y + (right.y - left.y) * amount,
            left.z + (right.z - left.z) * amount};
}
[[nodiscard]] Quat nlerp(Quat left, Quat right, const float amount) noexcept {
    if (dot(left, right) < 0.0F) { right.x = -right.x; right.y = -right.y; right.z = -right.z; right.w = -right.w; }
    return normalized({left.x + (right.x - left.x) * amount, left.y + (right.y - left.y) * amount,
                       left.z + (right.z - left.z) * amount, left.w + (right.w - left.w) * amount});
}
[[nodiscard]] Quat slerp(Quat left, Quat right, const float amount) noexcept {
    float cosine = dot(left, right);
    if (cosine < 0.0F) { cosine = -cosine; right.x = -right.x; right.y = -right.y; right.z = -right.z; right.w = -right.w; }
    if (cosine > 0.9995F) return nlerp(left, right, amount);
    cosine = std::clamp(cosine, -1.0F, 1.0F);
    const float angle = std::acos(cosine);
    const float sine = std::sin(angle);
    if (std::abs(sine) <= std::numeric_limits<float>::epsilon()) return left;
    const float left_weight = std::sin((1.0F - amount) * angle) / sine;
    const float right_weight = std::sin(amount * angle) / sine;
    return normalized({left.x * left_weight + right.x * right_weight,
                       left.y * left_weight + right.y * right_weight,
                       left.z * left_weight + right.z * right_weight,
                       left.w * left_weight + right.w * right_weight});
}

[[nodiscard]] Matrix from_asset_bone(const std::array<float, 12>& source) noexcept {
    // ALO stores three float4 source columns. Each output component is the
    // vector-left dot product against one column; transpose that operation to
    // the column-vector presentation convention without changing basis.
    return {source[0], source[4], source[8], 0.0F,
            source[1], source[5], source[9], 0.0F,
            source[2], source[6], source[10], 0.0F,
            source[3], source[7], source[11], 1.0F};
}
[[nodiscard]] Matrix inverse_affine(const Matrix& matrix) noexcept {
    const float a = matrix[0], b = matrix[4], c = matrix[8];
    const float d = matrix[1], e = matrix[5], f = matrix[9];
    const float g = matrix[2], h = matrix[6], i = matrix[10];
    const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!finite(determinant) || std::abs(determinant) <= std::numeric_limits<float>::epsilon()) {
        // ALO permits zero-scale bind axes. For the orthogonal TRS matrices
        // emitted by the accepted loader, this is the Moore-Penrose inverse:
        // each non-zero basis axis is inverted and a collapsed axis remains
        // collapsed. This keeps the palette finite without inventing scale.
        const float x2 = a * a + d * d + g * g;
        const float y2 = b * b + e * e + h * h;
        const float z2 = c * c + f * f + i * i;
        Matrix result{};
        if (x2 > std::numeric_limits<float>::epsilon()) {
            result[0] = a / x2; result[4] = d / x2; result[8] = g / x2;
        }
        if (y2 > std::numeric_limits<float>::epsilon()) {
            result[1] = b / y2; result[5] = e / y2; result[9] = h / y2;
        }
        if (z2 > std::numeric_limits<float>::epsilon()) {
            result[2] = c / z2; result[6] = f / z2; result[10] = i / z2;
        }
        result[15] = 1.0F;
        const Vec3 translation{matrix[12], matrix[13], matrix[14]};
        result[12] = -(result[0] * translation.x + result[4] * translation.y + result[8] * translation.z);
        result[13] = -(result[1] * translation.x + result[5] * translation.y + result[9] * translation.z);
        result[14] = -(result[2] * translation.x + result[6] * translation.y + result[10] * translation.z);
        return result;
    }
    const float inverse = 1.0F / determinant;
    Matrix result{
        (e * i - f * h) * inverse, (f * g - d * i) * inverse, (d * h - e * g) * inverse, 0.0F,
        (c * h - b * i) * inverse, (a * i - c * g) * inverse, (b * g - a * h) * inverse, 0.0F,
        (b * f - c * e) * inverse, (c * d - a * f) * inverse, (a * e - b * d) * inverse, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F};
    const Vec3 translation{matrix[12], matrix[13], matrix[14]};
    result[12] = -(result[0] * translation.x + result[4] * translation.y + result[8] * translation.z);
    result[13] = -(result[1] * translation.x + result[5] * translation.y + result[9] * translation.z);
    result[14] = -(result[2] * translation.x + result[6] * translation.y + result[10] * translation.z);
    return result;
}

// The allocation is the identity; no address or counter value is exposed.
struct PlayerOrigin final {};

struct Trs final { Vec3 translation; Vec3 scale; Quat rotation; };
[[nodiscard]] Trs decompose(const Matrix& matrix) noexcept {
    const Vec3 x{matrix[0], matrix[1], matrix[2]};
    const Vec3 y{matrix[4], matrix[5], matrix[6]};
    const Vec3 z{matrix[8], matrix[9], matrix[10]};
    const auto length = [](const Vec3 value) { return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z); };
    Vec3 scale{length(x), length(y), length(z)};
    if (scale.x <= std::numeric_limits<float>::epsilon()) scale.x = 1.0F;
    if (scale.y <= std::numeric_limits<float>::epsilon()) scale.y = 1.0F;
    if (scale.z <= std::numeric_limits<float>::epsilon()) scale.z = 1.0F;
    const float m00 = matrix[0] / scale.x, m01 = matrix[4] / scale.y, m02 = matrix[8] / scale.z;
    const float m10 = matrix[1] / scale.x, m11 = matrix[5] / scale.y, m12 = matrix[9] / scale.z;
    const float m20 = matrix[2] / scale.x, m21 = matrix[6] / scale.y, m22 = matrix[10] / scale.z;
    Quat rotation{};
    const float trace = m00 + m11 + m22;
    if (trace > 0.0F) { const float s = std::sqrt(trace + 1.0F) * 2.0F; rotation = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25F * s}; }
    else if (m00 > m11 && m00 > m22) { const float s = std::sqrt(1.0F + m00 - m11 - m22) * 2.0F; rotation = {0.25F * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s}; }
    else if (m11 > m22) { const float s = std::sqrt(1.0F + m11 - m00 - m22) * 2.0F; rotation = {(m01 + m10) / s, 0.25F * s, (m12 + m21) / s, (m02 - m20) / s}; }
    else { const float s = std::sqrt(1.0F + m22 - m00 - m11) * 2.0F; rotation = {(m02 + m20) / s, (m12 + m21) / s, 0.25F * s, (m10 - m01) / s}; }
    return {{matrix[12], matrix[13], matrix[14]}, scale, normalized(rotation)};
}
// decompose() is exact only for a proper TRS: it takes unsigned column lengths
// and replaces a collapsed one with 1. Returns the first failed rule of the
// intermediate bind-blend contract, or empty for a supported bind.
[[nodiscard]] std::string_view unsupported_bind(const Matrix& matrix) noexcept {
    const auto dot3 = [](const Vec3 left, const Vec3 right) { return left.x * right.x + left.y * right.y + left.z * right.z; };
    const Vec3 x{matrix[0], matrix[1], matrix[2]};
    const Vec3 y{matrix[4], matrix[5], matrix[6]};
    const Vec3 z{matrix[8], matrix[9], matrix[10]};
    const float x2 = dot3(x, x), y2 = dot3(y, y), z2 = dot3(z, z);
    if (!finite(x2) || !finite(y2) || !finite(z2)) return "bind axis length is non-finite";
    // The same threshold inverse_affine uses to treat an axis as collapsed.
    constexpr float collapsed = std::numeric_limits<float>::epsilon();
    if (x2 <= collapsed || y2 <= collapsed || z2 <= collapsed) return "bind axis is collapsed";
    const auto unit = [](const Vec3 value, const float length_squared) {
        const float inverse = 1.0F / std::sqrt(length_squared);
        return Vec3{value.x * inverse, value.y * inverse, value.z * inverse};
    };
    const Vec3 nx = unit(x, x2), ny = unit(y, y2), nz = unit(z, z2);
    // Admits binary32-rounded authored rotations; not a claim about the original game.
    constexpr float shear_tolerance = 1.0e-4F;
    if (std::abs(dot3(nx, ny)) > shear_tolerance || std::abs(dot3(nx, nz)) > shear_tolerance
        || std::abs(dot3(ny, nz)) > shear_tolerance) return "bind axes are sheared";
    const Vec3 cross{ny.y * nz.z - ny.z * nz.y, ny.z * nz.x - ny.x * nz.z, ny.x * nz.y - ny.y * nz.x};
    if (!(dot3(nx, cross) > 0.0F)) return "bind basis is mirrored";
    return {};
}
[[nodiscard]] bool positive_scale(const Vec3 value) noexcept {
    const auto positive = [](const float component) {
        return component > 0.0F && component * component > std::numeric_limits<float>::epsilon();
    };
    return positive(value.x) && positive(value.y) && positive(value.z);
}
[[nodiscard]] Matrix compose(const Trs& value) noexcept {
    const Quat q = normalized(value.rotation);
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return {(1.0F - 2.0F * (yy + zz)) * value.scale.x, (2.0F * (xy + wz)) * value.scale.x, (2.0F * (xz - wy)) * value.scale.x, 0.0F,
            (2.0F * (xy - wz)) * value.scale.y, (1.0F - 2.0F * (xx + zz)) * value.scale.y, (2.0F * (yz + wx)) * value.scale.y, 0.0F,
            (2.0F * (xz + wy)) * value.scale.z, (2.0F * (yz - wx)) * value.scale.z, (1.0F - 2.0F * (xx + yy)) * value.scale.z, 0.0F,
            value.translation.x, value.translation.y, value.translation.z, 1.0F};
}
[[nodiscard]] float interpolation_amount(const assets::Interpolation mode, const float fraction) noexcept {
    return mode == assets::Interpolation::step ? 0.0F : fraction;
}

} // namespace

struct Player::Bone final { std::string name; std::int32_t parent{}; Matrix bind_local{}, bind_model{}, inverse_bind{}; bool visible{}; };
struct Player::Track final { std::size_t bone{}; assets::AnimationTrack track; };

Player::~Player() = default;
Player::Player(Player&&) noexcept = default;
Player& Player::operator=(Player&&) noexcept = default;

Matrix Player::identity_matrix() noexcept { return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F}; }
Matrix Player::asset_to_render_matrix() noexcept { return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F}; }
Matrix Player::asset_to_render_transform(const Matrix& asset) noexcept {
    const Matrix conversion = asset_to_render_matrix();
    // The basis conversion is a proper rotation, so its inverse is its transpose.
    const Matrix inverse{conversion[0], conversion[4], conversion[8], 0.0F,
        conversion[1], conversion[5], conversion[9], 0.0F,
        conversion[2], conversion[6], conversion[10], 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F};
    return multiply(multiply(conversion, asset), inverse);
}
Matrix Player::multiply(const Matrix& left, const Matrix& right) noexcept {
    Matrix result{};
    for (std::size_t column = 0; column < 4; ++column) for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t index = 0; index < 4; ++index) result[column * 4 + row] += left[index * 4 + row] * right[column * 4 + index];
    return result;
}

assets::Vertex Player::rigid_vertex_to_bind(const Matrix& bind_model, const assets::Vertex& vertex) noexcept {
    const Vec3 x{bind_model[0], bind_model[1], bind_model[2]};
    const Vec3 y{bind_model[4], bind_model[5], bind_model[6]};
    const Vec3 z{bind_model[8], bind_model[9], bind_model[10]};
    const auto linear = [&](const Vec3 value) {
        return Vec3{x.x * value.x + y.x * value.y + z.x * value.z,
                    x.y * value.x + y.y * value.y + z.y * value.z,
                    x.z * value.x + y.z * value.y + z.z * value.z};
    };
    const auto cross = [](const Vec3 left, const Vec3 right) {
        return Vec3{left.y * right.z - left.z * right.y, left.z * right.x - left.x * right.z,
                    left.x * right.y - left.y * right.x};
    };
    // The cofactor matrix (columns y*z, z*x, x*y) is det * inverse transpose;
    // its sign is undone so a mirrored basis keeps outward normals outward.
    const Vec3 cx = cross(y, z), cy = cross(z, x), cz = cross(x, y);
    const float determinant = x.x * cx.x + x.y * cx.y + x.z * cx.z;
    const float sign = determinant < 0.0F ? -1.0F : 1.0F;
    assets::Vertex result = vertex;
    const Vec3 position = linear(vertex.position);
    result.position = {position.x + bind_model[12], position.y + bind_model[13], position.z + bind_model[14]};
    const Vec3& n = vertex.normal;
    result.normal = {sign * (cx.x * n.x + cy.x * n.y + cz.x * n.z),
                     sign * (cx.y * n.x + cy.y * n.y + cz.y * n.z),
                     sign * (cx.z * n.x + cy.z * n.y + cz.z * n.z)};
    result.tangent = linear(vertex.tangent);
    result.binormal = linear(vertex.binormal);
    return result;
}

core::Result<Player> Player::create(const assets::Model& model, const assets::Animation* animation) {
    Player result;
    result.origin_ = std::make_shared<const PlayerOrigin>();
    result.bones_.reserve(model.bones.size());
    for (std::size_t index = 0; index < model.bones.size(); ++index) {
        const auto& source = model.bones[index];
        if (source.parent < -1 || source.parent >= static_cast<std::int32_t>(index)) return core::Result<Player>::failure(failure(diagnostic_codes::invalid_model, "model hierarchy must be parent-first and acyclic", model.source.logical_path));
        const Matrix local = from_asset_bone(source.relative_transform);
        if (!finite(local)) return core::Result<Player>::failure(failure(diagnostic_codes::invalid_model, "model bind transform is non-finite", model.source.logical_path));
        const Matrix parent = source.parent < 0 ? identity_matrix() : result.bones_[static_cast<std::size_t>(source.parent)].bind_model;
        const Matrix model_transform = multiply(parent, local);
        result.bones_.push_back({source.name, source.parent, local, model_transform,
            inverse_affine(model_transform), source.visible});
    }
    if (!animation) return core::Result<Player>::success(std::move(result));
    if (!finite(animation->frames_per_second) || animation->frames_per_second <= 0.0F
        || !finite(animation->duration_seconds) || animation->playable_frame_count >= animation->stored_frame_count) {
        return core::Result<Player>::failure(failure(diagnostic_codes::invalid_animation, "animation duration/frame metadata is invalid", animation->source.logical_path));
    }
    const float expected_duration = static_cast<float>(animation->playable_frame_count)
        / animation->frames_per_second;
    const float duration_tolerance = std::max(0.00001F, expected_duration * 0.00001F);
    if (std::abs(animation->duration_seconds - expected_duration) > duration_tolerance) {
        return core::Result<Player>::failure(failure(diagnostic_codes::invalid_animation,
            "animation duration does not match playable frames and frame rate",
            animation->source.logical_path));
    }
    result.frames_per_second_ = animation->frames_per_second;
    result.duration_seconds_ = animation->duration_seconds;
    result.playable_frames_ = animation->playable_frame_count;
    std::unordered_set<std::size_t> bound_bones;
    for (const assets::AnimationTrack& source : animation->tracks) {
        const auto valid_interpolation = [](const assets::Interpolation value) {
            return value == assets::Interpolation::step || value == assets::Interpolation::linear
                || value == assets::Interpolation::spherical;
        };
        if (source.bone_index >= result.bones_.size() || source.bone_index >= model.bones.size()
            || source.bone_name != model.bones[source.bone_index].name || source.samples.size() != animation->stored_frame_count
            || !bound_bones.insert(source.bone_index).second
            || !valid_interpolation(source.translation_interpolation)
            || !valid_interpolation(source.scale_interpolation)
            || !valid_interpolation(source.rotation_interpolation)
            || source.visibility_interpolation != assets::Interpolation::step) {
            return core::Result<Player>::failure(failure(diagnostic_codes::invalid_animation, "animation track does not map exactly to model bone name/index", animation->source.logical_path));
        }
        for (const auto& sample : source.samples) if (!finite(sample.translation) || !finite(sample.scale) || !finite(sample.rotation))
            return core::Result<Player>::failure(failure(diagnostic_codes::invalid_animation, "animation track has non-finite sample", animation->source.logical_path));
        result.tracks_.push_back({source.bone_index, source});
    }
    return core::Result<Player>::success(std::move(result));
}

core::Result<Pose> Player::sample(const SampleRequest& request) const {
    if (!finite(request.time_seconds) || !finite(request.blend_to_bind) || request.blend_to_bind < 0.0F || request.blend_to_bind > 1.0F)
        return core::Result<Pose>::failure(failure(diagnostic_codes::invalid_request, "sample request requires finite time and blend in [0, 1]"));
    float sampled = std::max(request.time_seconds, 0.0F);
    if (request.mode == PlaybackMode::loop && duration_seconds_ > 0.0F) sampled = std::fmod(sampled, duration_seconds_);
    else sampled = std::min(sampled, duration_seconds_);
    if (request.blend_to_bind == 1.0F) {
        Pose result;
        result.origin_ = origin_;
        result.sampled_time_seconds = sampled;
        result.bones.resize(bones_.size());
        for (std::size_t index = 0; index < bones_.size(); ++index) {
            const Bone& bone = bones_[index];
            result.bones[index].local_asset = bone.bind_local;
            result.bones[index].model_asset = bone.bind_model;
            result.bones[index].skin_asset = multiply(bone.bind_model, bone.inverse_bind);
            result.bones[index].visible = bone.visible;
        }
        return core::Result<Pose>::success(std::move(result));
    }
    const float frame = frames_per_second_ > 0.0F ? sampled * frames_per_second_ : 0.0F;
    const std::size_t first = playable_frames_ == 0 ? 0U : std::min<std::size_t>(static_cast<std::size_t>(frame), playable_frames_);
    const std::size_t second = playable_frames_ == 0 ? 0U : std::min(first + 1U, static_cast<std::size_t>(playable_frames_));
    const float fraction = std::clamp(frame - static_cast<float>(first), 0.0F, 1.0F);
    return interpolate(first, second, fraction, sampled, request.blend_to_bind);
}

core::Result<Pose> Player::sample_tick(const std::uint64_t tick, const std::uint32_t ticks_per_second) const {
    if (ticks_per_second == 0)
        return core::Result<Pose>::failure(failure(diagnostic_codes::invalid_request, "tick sampling requires a positive tick rate"));
    if (playable_frames_ == 0) return interpolate(0U, 0U, 0.0F, 0.0F, 0.0F);
    constexpr float largest_rate = 1000000.0F;
    if (!(frames_per_second_ >= 1.0F && frames_per_second_ <= largest_rate) || std::floor(frames_per_second_) != frames_per_second_)
        return core::Result<Pose>::failure(failure(diagnostic_codes::invalid_request, "tick sampling requires an integral frame rate"));
    const auto rate = static_cast<std::uint64_t>(frames_per_second_);
    const std::uint64_t ticks = ticks_per_second;
    constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
    // Positions are counted in 1/ticks_per_second frames: one loop is
    // playable * ticks of them, and tick n sits at n * rate of them.
    if (playable_frames_ > most / ticks || playable_frames_ * ticks - 1U > most / rate)
        return core::Result<Pose>::failure(failure(diagnostic_codes::invalid_request, "tick sampling position does not fit 64 bits"));
    const std::uint64_t period = playable_frames_ * ticks;
    const std::uint64_t position = (tick % period) * rate % period;
    const auto first = static_cast<std::size_t>(position / ticks);
    const std::size_t second = std::min(first + 1U, static_cast<std::size_t>(playable_frames_));
    const float fraction = static_cast<float>(position % ticks) / static_cast<float>(ticks);
    const auto sampled = static_cast<float>(static_cast<double>(position) / (static_cast<double>(ticks) * static_cast<double>(rate)));
    return interpolate(first, second, fraction, sampled, 0.0F);
}

core::Result<Pose> Player::sample_position(const std::uint64_t position, const std::uint32_t subdivisions) const {
    Pose output;
    auto sampled = sample_position(position, subdivisions, output);
    if (!sampled) return core::Result<Pose>::failure(std::move(sampled.error()));
    return core::Result<Pose>::success(std::move(output));
}

core::Result<void> Player::sample_position(const std::uint64_t position, const std::uint32_t subdivisions,
    Pose& output) const {
    if (subdivisions == 0)
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_request, "position sampling requires a positive subdivision count"));
    if (playable_frames_ == 0) return interpolate(0U, 0U, 0.0F, 0.0F, 0.0F, output);
    const std::uint64_t whole = position / subdivisions;
    if (whole > playable_frames_ || (whole == playable_frames_ && position % subdivisions != 0))
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_request, "clip position is past the last frame"));
    const auto first = static_cast<std::size_t>(whole);
    const std::size_t second = std::min(first + 1U, static_cast<std::size_t>(playable_frames_));
    const float fraction = static_cast<float>(position % subdivisions) / static_cast<float>(subdivisions);
    const auto sampled = frames_per_second_ > 0.0F
        ? static_cast<float>(static_cast<double>(position) / (static_cast<double>(subdivisions) * static_cast<double>(frames_per_second_)))
        : 0.0F;
    return interpolate(first, second, fraction, sampled, 0.0F, output);
}

std::uint32_t Player::playable_frames() const noexcept { return playable_frames_; }
float Player::frames_per_second() const noexcept { return frames_per_second_; }

core::Result<Pose> Player::interpolate(const std::size_t first, const std::size_t second, const float fraction, const float sampled,
    const float blend_to_bind) const {
    Pose result;
    auto status = interpolate(first, second, fraction, sampled, blend_to_bind, result);
    if (!status) return core::Result<Pose>::failure(std::move(status.error()));
    return core::Result<Pose>::success(std::move(result));
}

core::Result<void> Player::interpolate(const std::size_t first, const std::size_t second, const float fraction, const float sampled,
    const float blend_to_bind, Pose& result) const {
    result.origin_ = origin_; result.sampled_time_seconds = sampled; result.bones.resize(bones_.size());
    for (std::size_t index = 0; index < bones_.size(); ++index) { result.bones[index].local_asset = bones_[index].bind_local; result.bones[index].visible = bones_[index].visible; }
    for (const Track& binding : tracks_) {
        const auto& a = binding.track.samples[first]; const auto& b = binding.track.samples[second];
        Trs value{lerp(a.translation, b.translation, interpolation_amount(binding.track.translation_interpolation, fraction)),
                  lerp(a.scale, b.scale, interpolation_amount(binding.track.scale_interpolation, fraction)),
                  binding.track.rotation_interpolation == assets::Interpolation::spherical
                    ? slerp(normalized(a.rotation), normalized(b.rotation), fraction)
                    : nlerp(normalized(a.rotation), normalized(b.rotation), interpolation_amount(binding.track.rotation_interpolation, fraction))};
        // Intermediate blends support only a proper TRS on both ends; see
        // docs/asset-formats.md#presentation-animation-and-attachments.
        std::string_view unsupported;
        if (blend_to_bind > 0.0F) {
            unsupported = unsupported_bind(bones_[binding.bone].bind_local);
            if (unsupported.empty() && !positive_scale(value.scale)) unsupported = "animated scale is not positive";
            if (unsupported.empty()) { const Trs bind = decompose(bones_[binding.bone].bind_local); value.translation = lerp(value.translation, bind.translation, blend_to_bind); value.scale = lerp(value.scale, bind.scale, blend_to_bind); value.rotation = slerp(value.rotation, bind.rotation, blend_to_bind); }
        }
        const Matrix local = compose(value);
        if (blend_to_bind > 0.0F && unsupported.empty() && !finite(local)) unsupported = "blended local transform is non-finite";
        if (!unsupported.empty())
            return core::Result<void>::failure(failure(diagnostic_codes::unsupported_bind_blend,
                "intermediate bind blend is unsupported for tracked bone '" + bones_[binding.bone].name + "': "
                + std::string(unsupported)));
        result.bones[binding.bone].local_asset = local;
        result.bones[binding.bone].visible = a.visible;
    }
    for (std::size_t index = 0; index < bones_.size(); ++index) {
        const Matrix parent = bones_[index].parent < 0 ? identity_matrix() : result.bones[static_cast<std::size_t>(bones_[index].parent)].model_asset;
        result.bones[index].model_asset = multiply(parent, result.bones[index].local_asset);
        result.bones[index].skin_asset = multiply(result.bones[index].model_asset, bones_[index].inverse_bind);
    }
    return core::Result<void>::success();
}

bool Player::sampled(const Pose& pose) const noexcept {
    // Owner equivalence compares control blocks, which a live weak reference
    // keeps from being reused, so a destroyed player's identity never aliases
    // a later one. An empty origin (default pose, moved-from player) never matches.
    return origin_ && !origin_.owner_before(pose.origin_) && !pose.origin_.owner_before(origin_);
}
std::size_t Player::bone_count() const noexcept { return bones_.size(); }

core::Result<AttachmentTransform> Player::attachment(const Pose& pose, const std::string_view bone_name,
    const AttachmentSpace space, const Matrix& render_world) const {
    if (!sampled(pose))
        return core::Result<AttachmentTransform>::failure(failure(diagnostic_codes::invalid_request,
            "pose was not sampled by this player"));
    if (pose.bones.size() != bones_.size()) return core::Result<AttachmentTransform>::failure(failure(diagnostic_codes::invalid_request, "pose bone count does not match this player"));
    const auto found = std::find_if(bones_.begin(), bones_.end(), [bone_name](const Bone& bone) { return bone.name == bone_name; });
    if (found == bones_.end()) return core::Result<AttachmentTransform>::failure(failure(diagnostic_codes::missing_attachment, "named attachment bone is absent: " + std::string(bone_name)));
    if (std::find_if(std::next(found), bones_.end(), [bone_name](const Bone& bone) { return bone.name == bone_name; }) != bones_.end())
        return core::Result<AttachmentTransform>::failure(failure(diagnostic_codes::invalid_request, "named attachment bone is ambiguous: " + std::string(bone_name)));
    if (!finite(render_world)) return core::Result<AttachmentTransform>::failure(failure(diagnostic_codes::invalid_request, "world attachment transform is non-finite"));
    const std::size_t index = static_cast<std::size_t>(std::distance(bones_.begin(), found));
    if (!finite(pose.bones[index].model_asset))
        return core::Result<AttachmentTransform>::failure(failure(diagnostic_codes::invalid_request,
            "attachment bone model transform is non-finite"));
    Matrix transformed = asset_to_render_transform(pose.bones[index].model_asset);
    if (!finite(transformed))
        return core::Result<AttachmentTransform>::failure(failure(diagnostic_codes::invalid_request,
            "converted attachment transform is non-finite"));
    if (space == AttachmentSpace::world) {
        transformed = multiply(render_world, transformed);
        if (!finite(transformed))
            return core::Result<AttachmentTransform>::failure(failure(diagnostic_codes::invalid_request,
                "world attachment result is non-finite"));
    }
    return core::Result<AttachmentTransform>::success({space, transformed});
}

} // namespace eawr::presentation::animation
