#pragma once

// CPU vertex-deformation probe for explicitly named animation samples.
//
// Test and evidence tooling only.  It evaluates the indexed drawable vertices
// of one exact mesh/submesh of a parsed ALO under Player poses, using
// skinning routes equivalent to the Godot adapter's upload_mesh:
//
//   palette    submesh.skin_bones is non-empty.  Each of the four influences is
//              used unchanged: weight w (finite, >= 0) and local index l.  An
//              influence is active when w != 0; an active l must be inside
//              skin_bones and its global bone inside the model.  An inactive
//              influence is ignored whatever its index.  Weights are never
//              normalized.
//   rigid      skin_bones is empty and mesh.bone >= 0: one synthesized
//              influence of weight 1 on mesh.bone.  The vertex is stored in
//              that bone's space; upload_mesh moves it to bind space and the
//              skin palette moves it on, which is the bone's model transform
//              applied to the stored position.
//   unskinned  skin_bones is empty and mesh.bone == -1: positions pass through.
//
// The probe applies the renderer's weight and palette rules to the selected
// submesh, plus validation of its own that upload_mesh does not perform: finite
// source positions, a non-empty triangle-list index buffer within range, a
// mesh bone of at least -1 (upload_mesh treats bone < -1 as unskinned),
// finite affine pose matrices, and no float overflow in weight sums, deformed
// positions or reported distances.  It is therefore not identical to upload
// validation, and it validates the selected submesh only.
//
// Positions stay in the ALO asset basis.  A vertex whose palette weights sum to
// zero or to a non-unit value has no CPU prediction: what a GPU does with such
// weights is not modelled here, so the probe records the raw influences and
// emits no position.  Nothing here claims GPU or original-game parity.

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/animation/animation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::tests::animation_deformation {

namespace playback = eawr::presentation::animation;
using Vec3 = assets::Vec3f;

// A weight sum within this distance of 1 is a unit sum.  Anything else is
// reported as non-unit and gets no prediction.
inline constexpr float unit_weight_tolerance = 1.0e-5F;
// A predicted vertex whose animated position differs from its bind position by
// more than this Euclidean distance (asset units) counts as changed.
inline constexpr float change_epsilon = 1.0e-4F;

template <class T>
struct Checked final {
    std::optional<T> value;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return value.has_value(); }
    [[nodiscard]] static Checked ok(T result) { return {std::move(result), {}}; }
    [[nodiscard]] static Checked fail(std::string message) { return {std::nullopt, std::move(message)}; }
};

struct Selection final {
    std::string mesh;
    std::size_t submesh{};
};

enum class Route : std::uint8_t { palette, rigid, unskinned };
enum class WeightClass : std::uint8_t { unit, nonunit, zero, synthesized, none };

[[nodiscard]] constexpr std::string_view to_string(const Route route) noexcept {
    switch (route) {
    case Route::palette: return "palette";
    case Route::rigid: return "rigid";
    case Route::unskinned: return "unskinned";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::string_view to_string(const WeightClass value) noexcept {
    switch (value) {
    case WeightClass::unit: return "unit";
    case WeightClass::nonunit: return "nonunit";
    case WeightClass::zero: return "zero";
    case WeightClass::synthesized: return "synthesized";
    case WeightClass::none: return "none";
    }
    return "unknown";
}

struct Influence final {
    std::uint32_t local{};
    std::uint32_t global{};
    float weight{};
    bool active{};
};

struct DeformedVertex final {
    std::uint32_t index{};
    Vec3 source;
    WeightClass weight_class{WeightClass::none};
    float weight_sum{};
    std::array<Influence, 4> influences{};
    std::uint8_t active_influences{};
    std::optional<Vec3> position;
};

struct Deformed final {
    Route route{Route::unskinned};
    std::string mesh_name;
    std::int32_t mesh_bone{-1};
    std::string shader;
    std::size_t vertex_count{};
    std::size_t index_count{};
    std::vector<std::uint32_t> skin_bones;
    // Unique indexed vertices, ascending by vertex index.
    std::vector<DeformedVertex> vertices;
};

// Resolves an exact, unique, case-sensitive mesh name and a submesh index.
[[nodiscard]] inline Checked<std::pair<std::size_t, std::size_t>> resolve(
    const assets::Model& model, const Selection& selection) {
    using Out = Checked<std::pair<std::size_t, std::size_t>>;
    std::optional<std::size_t> found;
    for (std::size_t index = 0; index < model.meshes.size(); ++index) {
        if (model.meshes[index].name != selection.mesh) continue;
        if (found) return Out::fail("mesh name is ambiguous: " + selection.mesh);
        found = index;
    }
    if (!found) return Out::fail("mesh is absent: " + selection.mesh);
    const assets::Mesh& mesh = model.meshes[*found];
    if (!mesh.visible) return Out::fail("mesh is not drawable (invisible): " + selection.mesh);
    if (selection.submesh >= mesh.submeshes.size()) return Out::fail("submesh index is out of range");
    return Out::ok({*found, selection.submesh});
}

[[nodiscard]] inline bool finite(const Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

// Column-major, column-vector transform of an asset-space point.
[[nodiscard]] inline Vec3 transform_point(const playback::Matrix& matrix, const Vec3 point) noexcept {
    return {matrix[0] * point.x + matrix[4] * point.y + matrix[8] * point.z + matrix[12],
            matrix[1] * point.x + matrix[5] * point.y + matrix[9] * point.z + matrix[13],
            matrix[2] * point.x + matrix[6] * point.y + matrix[10] * point.z + matrix[14]};
}

// The one asset-to-render conversion, for points: Player::asset_to_render_matrix
// applied to a position.  The probe's results stay in asset space; this exists
// so the conversion identity can be tested against the renderer's convention.
[[nodiscard]] inline Vec3 asset_to_render_point(const Vec3 point) noexcept {
    return transform_point(playback::Player::asset_to_render_matrix(), point);
}

[[nodiscard]] inline std::optional<std::string> check_pose(
    const assets::Model& model, const playback::Pose& pose) {
    if (pose.bones.size() != model.bones.size()) return "pose bone count does not match the model";
    for (const playback::BonePose& bone : pose.bones) {
        const playback::Matrix& m = bone.skin_asset;
        if (!std::all_of(m.begin(), m.end(), [](const float value) { return std::isfinite(value); }))
            return "pose contains a non-finite skin matrix";
        if (m[3] != 0.0F || m[7] != 0.0F || m[11] != 0.0F || m[15] != 1.0F)
            return "pose contains a non-affine skin matrix";
    }
    return std::nullopt;
}

// Deforms the indexed vertices of one submesh by the pose's skin palette.
// Every vertex of the submesh is validated (the renderer's upload also walks
// every vertex), but only vertices referenced by the index buffer are evaluated.
[[nodiscard]] inline Checked<Deformed> deform(
    const assets::Model& model, const Selection& selection, const playback::Pose& pose) {
    using Out = Checked<Deformed>;
    const auto resolved = resolve(model, selection);
    if (!resolved) return Out::fail(resolved.error);
    const assets::Mesh& mesh = model.meshes[resolved.value->first];
    const assets::Submesh& submesh = mesh.submeshes[resolved.value->second];
    if (const auto error = check_pose(model, pose)) return Out::fail(*error);

    Deformed result;
    result.mesh_name = mesh.name;
    result.mesh_bone = mesh.bone;
    result.shader = submesh.shader;
    result.vertex_count = submesh.vertices.size();
    result.index_count = submesh.indices.size();
    result.skin_bones = submesh.skin_bones;
    if (mesh.bone < -1) return Out::fail("mesh bone is negative but not -1");
    if (!submesh.skin_bones.empty()) result.route = Route::palette;
    else if (mesh.bone >= 0) result.route = Route::rigid;
    else result.route = Route::unskinned;
    if (result.route != Route::unskinned && model.bones.empty())
        return Out::fail("skinned submesh on a model without bones");
    if (result.route == Route::rigid && static_cast<std::size_t>(mesh.bone) >= model.bones.size())
        return Out::fail("rigid mesh bone is out of range");

    if (submesh.vertices.empty()) return Out::fail("submesh has no vertices");
    if (submesh.indices.empty() || submesh.indices.size() % 3U != 0U)
        return Out::fail("index buffer is empty or not a triangle list");
    std::set<std::uint32_t> referenced;
    for (const std::uint16_t index : submesh.indices) {
        if (index >= submesh.vertices.size()) return Out::fail("index exceeds the vertex buffer");
        referenced.insert(index);
    }

    // Validate every vertex first: the renderer's weight and palette checks,
    // plus the probe's own finite-position check.
    for (const assets::Vertex& vertex : submesh.vertices) {
        if (!finite(vertex.position)) return Out::fail("vertex position is non-finite");
        if (result.route != Route::palette) continue;
        for (std::size_t influence = 0; influence < 4; ++influence) {
            const float weight = vertex.bone_weights[influence];
            const std::uint32_t local = vertex.bone_indices[influence];
            if (!std::isfinite(weight)) return Out::fail("vertex weight is non-finite");
            if (weight < 0.0F) return Out::fail("vertex weight is negative");
            if (weight == 0.0F) continue;
            if (local >= submesh.skin_bones.size()) return Out::fail("active local bone index is outside the skin palette");
            if (submesh.skin_bones[local] >= model.bones.size()) return Out::fail("skin palette bone is outside the model");
        }
    }

    result.vertices.reserve(referenced.size());
    for (const std::uint32_t index : referenced) {
        const assets::Vertex& vertex = submesh.vertices[index];
        DeformedVertex out;
        out.index = index;
        out.source = vertex.position;
        if (result.route == Route::unskinned) {
            out.weight_class = WeightClass::none;
            out.position = vertex.position;
        } else if (result.route == Route::rigid) {
            out.weight_class = WeightClass::synthesized;
            out.weight_sum = 1.0F;
            out.influences[0] = {0U, static_cast<std::uint32_t>(mesh.bone), 1.0F, true};
            out.active_influences = 1;
            out.position = transform_point(pose.bones[static_cast<std::size_t>(mesh.bone)].model_asset, vertex.position);
        } else {
            float sum = 0.0F;
            Vec3 blended{};
            for (std::size_t influence = 0; influence < 4; ++influence) {
                const float weight = vertex.bone_weights[influence];
                Influence& record = out.influences[influence];
                record.local = vertex.bone_indices[influence];
                record.weight = weight;
                record.active = weight != 0.0F;
                if (!record.active) continue;
                record.global = submesh.skin_bones[record.local];
                ++out.active_influences;
                sum += weight;
                const Vec3 moved = transform_point(pose.bones[record.global].skin_asset, vertex.position);
                blended.x += weight * moved.x;
                blended.y += weight * moved.y;
                blended.z += weight * moved.z;
            }
            if (!std::isfinite(sum)) return Out::fail("vertex weight sum overflows");
            out.weight_sum = sum;
            if (sum == 0.0F) out.weight_class = WeightClass::zero;
            else if (std::abs(sum - 1.0F) <= unit_weight_tolerance) out.weight_class = WeightClass::unit;
            else out.weight_class = WeightClass::nonunit;
            if (out.weight_class == WeightClass::unit) out.position = blended;
        }
        if (out.position && !finite(*out.position)) return Out::fail("deformed position overflows");
        result.vertices.push_back(out);
    }
    return Out::ok(std::move(result));
}

struct Bounds final {
    Vec3 min{};
    Vec3 max{};
    bool empty{true};
    void add(const Vec3 point) noexcept {
        if (empty) { min = max = point; empty = false; return; }
        min = {std::min(min.x, point.x), std::min(min.y, point.y), std::min(min.z, point.z)};
        max = {std::max(max.x, point.x), std::max(max.y, point.y), std::max(max.z, point.z)};
    }
};

struct VertexComparison final {
    DeformedVertex vertex;          // carries the bind position
    std::optional<Vec3> animated;
    float displacement{};
    bool changed{};
};

struct BoneDetail final {
    std::uint32_t bone{};
    std::string name;
    float bind_skin_deviation{};      // max |S - I| element of the bind palette
    float animated_skin_deviation{};  // max |S - I| element of the animated palette
};

[[nodiscard]] inline float identity_deviation(const playback::Matrix& matrix) noexcept {
    const playback::Matrix identity = playback::Player::identity_matrix();
    float result = 0.0F;
    for (std::size_t index = 0; index < matrix.size(); ++index)
        result = std::max(result, std::abs(matrix[index] - identity[index]));
    return result;
}

struct Evaluation final {
    Route route{Route::unskinned};
    std::string mesh_name;
    std::int32_t mesh_bone{-1};
    std::string shader;
    std::size_t vertex_count{};
    std::size_t index_count{};
    std::vector<std::uint32_t> skin_bones;
    std::vector<VertexComparison> vertices;
    // Global bones carrying at least one active influence (or the rigid bone).
    std::vector<std::uint32_t> influencing_bones;
    std::vector<BoneDetail> influencing_detail;
    std::size_t unit{}, nonunit{}, zero{}, synthesized{}, none{};
    std::array<std::size_t, 5> active_histogram{};  // vertices with 0..4 active influences
    float max_unit_deviation{};                      // max |sum - 1| among unit vertices
    std::size_t predicted{};
    std::size_t changed{};
    float max_displacement{};
    std::optional<std::uint32_t> max_displacement_vertex;
    double mean_displacement{};
    float max_bind_residual{};                       // max |bind - source|
    Bounds bind_bounds;
    Bounds animated_bounds;
};

// Euclidean distance in double, narrowed to float only when it fits.  Two
// finite float points can be further apart than the largest float; such a
// distance is refused rather than reported as infinity.
[[nodiscard]] inline std::optional<float> distance(const Vec3 a, const Vec3 b) noexcept {
    const double dx = static_cast<double>(a.x) - b.x;
    const double dy = static_cast<double>(a.y) - b.y;
    const double dz = static_cast<double>(a.z) - b.z;
    const double result = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(result) || result > static_cast<double>(std::numeric_limits<float>::max())) return std::nullopt;
    return static_cast<float>(result);
}

// Deforms the submesh under a bind pose and an animated pose and compares them.
// Passing the bind pose as the animated pose is the substituted-bind control.
[[nodiscard]] inline Checked<Evaluation> evaluate(const assets::Model& model, const Selection& selection,
    const playback::Pose& bind_pose, const playback::Pose& animated_pose) {
    using Out = Checked<Evaluation>;
    auto bind = deform(model, selection, bind_pose);
    if (!bind) return Out::fail("bind pose: " + bind.error);
    auto animated = deform(model, selection, animated_pose);
    if (!animated) return Out::fail("animated pose: " + animated.error);
    Evaluation result;
    result.route = bind.value->route;
    result.mesh_name = bind.value->mesh_name;
    result.mesh_bone = bind.value->mesh_bone;
    result.shader = bind.value->shader;
    result.vertex_count = bind.value->vertex_count;
    result.index_count = bind.value->index_count;
    result.skin_bones = bind.value->skin_bones;
    std::set<std::uint32_t> influencing;
    double displacement_sum = 0.0;
    for (std::size_t index = 0; index < bind.value->vertices.size(); ++index) {
        const DeformedVertex& at_bind = bind.value->vertices[index];
        const DeformedVertex& at_pose = animated.value->vertices[index];
        VertexComparison compared{at_bind, at_pose.position, 0.0F, false};
        switch (at_bind.weight_class) {
        case WeightClass::unit: ++result.unit;
            result.max_unit_deviation = std::max(result.max_unit_deviation, std::abs(at_bind.weight_sum - 1.0F)); break;
        case WeightClass::nonunit: ++result.nonunit; break;
        case WeightClass::zero: ++result.zero; break;
        case WeightClass::synthesized: ++result.synthesized; break;
        case WeightClass::none: ++result.none; break;
        }
        ++result.active_histogram[at_bind.active_influences];
        for (const Influence& influence : at_bind.influences) if (influence.active) influencing.insert(influence.global);
        if (at_bind.position && at_pose.position) {
            ++result.predicted;
            const auto displacement = distance(*at_bind.position, *at_pose.position);
            if (!displacement) return Out::fail("animated displacement overflows float");
            const auto residual = distance(*at_bind.position, at_bind.source);
            if (!residual) return Out::fail("bind residual overflows float");
            compared.displacement = *displacement;
            compared.changed = compared.displacement > change_epsilon;
            if (compared.changed) ++result.changed;
            displacement_sum += compared.displacement;
            if (!result.max_displacement_vertex || compared.displacement > result.max_displacement) {
                result.max_displacement = compared.displacement;
                result.max_displacement_vertex = at_bind.index;
            }
            result.max_bind_residual = std::max(result.max_bind_residual, *residual);
            result.bind_bounds.add(*at_bind.position);
            result.animated_bounds.add(*at_pose.position);
        }
        result.vertices.push_back(compared);
    }
    result.influencing_bones.assign(influencing.begin(), influencing.end());
    for (const std::uint32_t bone : result.influencing_bones)
        result.influencing_detail.push_back({bone, model.bones[bone].name,
            identity_deviation(bind_pose.bones[bone].skin_asset), identity_deviation(animated_pose.bones[bone].skin_asset)});
    if (result.predicted != 0U) result.mean_displacement = displacement_sum / static_cast<double>(result.predicted);
    return Out::ok(std::move(result));
}

// What a zero changed count means.  "Unchanged at this sample" is claimed only
// when every indexed vertex has a prediction; with no prediction the result is
// indeterminate, and with some unpredicted vertices it covers the predicted
// subset only.
[[nodiscard]] inline std::string_view unchanged_scope(const Evaluation& evaluation) noexcept {
    if (evaluation.changed != 0U) return "not_applicable";
    if (evaluation.predicted == 0U) return "indeterminate_no_prediction";
    if (evaluation.predicted != evaluation.vertices.size()) return "unchanged_predicted_subset_only";
    return "unchanged_at_this_sample_only";
}

// ---------------------------------------------------------------------------
// Receipt writing.  Deterministic: fixed key order, %.9g floats, no clock.

[[nodiscard]] inline std::string json_string(const std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (byte < 0x20U || byte == 0x7FU) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(byte));
            out += buffer;
        } else out += c;
    }
    return out + "\"";
}
[[nodiscard]] inline std::string json_number(const double value) {
    if (!std::isfinite(value)) return "null";
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.9g", value);
    return buffer;
}
[[nodiscard]] inline std::string json_vec(const Vec3 value) {
    return "[" + json_number(value.x) + "," + json_number(value.y) + "," + json_number(value.z) + "]";
}
[[nodiscard]] inline std::string json_bounds(const Bounds& bounds) {
    if (bounds.empty) return "null";
    return "{\"min\":" + json_vec(bounds.min) + ",\"max\":" + json_vec(bounds.max) + "}";
}

struct AssetIdentity final {
    std::string requested_path;
    std::string canonical_path;
    std::string sha256;
    std::string layer;
    std::string origin;
    std::string source_id;
    std::string original_path;
    std::uint64_t size{};
};

struct Header final {
    std::string compiler;
    std::string config;
    std::vector<std::pair<std::string, std::string>> tool_sources;
    std::vector<std::pair<std::string, std::string>> mounts;
    AssetIdentity model;
    std::optional<AssetIdentity> animation;   // empty: bind_only
    std::string mesh;
    std::size_t submesh{};
    float requested_time{};
    std::string mode;                          // loop, clamp or bind_only
    float sampled_time{};
    std::size_t bone_count{};
    std::size_t track_count{};
    std::vector<std::uint32_t> tracked_bones;  // bone indices carrying an ALA track
};

inline void write_identity(std::ostream& out, const AssetIdentity& identity) {
    out << "{\"requested_path\":" << json_string(identity.requested_path)
        << ",\"canonical_path\":" << json_string(identity.canonical_path)
        << ",\"sha256\":" << json_string(identity.sha256)
        << ",\"layer\":" << json_string(identity.layer)
        << ",\"origin\":" << json_string(identity.origin)
        << ",\"source_id\":" << json_string(identity.source_id)
        << ",\"original_path\":" << json_string(identity.original_path)
        << ",\"size\":" << identity.size << "}";
}

// The vertex block is written separately so its digest can be quoted in a
// sanitized report without the positions themselves.
[[nodiscard]] inline std::string vertex_block(const Evaluation& evaluation) {
    std::string out = "[";
    bool first = true;
    for (const VertexComparison& compared : evaluation.vertices) {
        const DeformedVertex& vertex = compared.vertex;
        if (!first) out += ",";
        first = false;
        out += "\n    {\"index\":" + std::to_string(vertex.index)
            + ",\"weight_class\":" + json_string(to_string(vertex.weight_class))
            + ",\"weight_sum\":" + json_number(vertex.weight_sum) + ",\"influences\":[";
        for (std::size_t influence = 0; influence < 4; ++influence) {
            const Influence& value = vertex.influences[influence];
            if (influence != 0) out += ",";
            out += "{\"local\":" + std::to_string(value.local) + ",\"weight\":" + json_number(value.weight)
                + ",\"active\":" + (value.active ? "true" : "false");
            if (value.active) out += ",\"global\":" + std::to_string(value.global);
            out += "}";
        }
        out += "],\"source\":" + json_vec(vertex.source)
            + ",\"bind\":" + (vertex.position ? json_vec(*vertex.position) : std::string("null"))
            + ",\"animated\":" + (compared.animated ? json_vec(*compared.animated) : std::string("null"))
            + ",\"displacement\":" + (vertex.position && compared.animated ? json_number(compared.displacement) : std::string("null"))
            + ",\"changed\":" + (compared.changed ? "true" : "false") + "}";
    }
    return out + "\n  ]";
}

// Writes the summary object (everything but per-vertex data).
inline void write_summary(std::ostream& out, const Evaluation& evaluation) {
    out << "{\"route\":" << json_string(to_string(evaluation.route))
        << ",\"mesh_name\":" << json_string(evaluation.mesh_name)
        << ",\"mesh_bone\":" << evaluation.mesh_bone
        << ",\"shader\":" << json_string(evaluation.shader)
        << ",\"vertex_count\":" << evaluation.vertex_count
        << ",\"index_count\":" << evaluation.index_count
        << ",\"indexed_vertex_count\":" << evaluation.vertices.size()
        << ",\"skin_bones\":[";
    for (std::size_t index = 0; index < evaluation.skin_bones.size(); ++index)
        out << (index ? "," : "") << evaluation.skin_bones[index];
    out << "],\"influencing_bones\":[";
    for (std::size_t index = 0; index < evaluation.influencing_bones.size(); ++index)
        out << (index ? "," : "") << evaluation.influencing_bones[index];
    out << "],\"influencing_bone_detail\":[";
    for (std::size_t index = 0; index < evaluation.influencing_detail.size(); ++index) {
        const BoneDetail& detail = evaluation.influencing_detail[index];
        out << (index ? "," : "") << "{\"bone\":" << detail.bone << ",\"name\":" << json_string(detail.name)
            << ",\"bind_skin_max_deviation\":" << json_number(detail.bind_skin_deviation)
            << ",\"animated_skin_max_deviation\":" << json_number(detail.animated_skin_deviation) << "}";
    }
    out << "],\"weights\":{\"unit\":" << evaluation.unit << ",\"nonunit\":" << evaluation.nonunit
        << ",\"zero\":" << evaluation.zero << ",\"synthesized\":" << evaluation.synthesized
        << ",\"none\":" << evaluation.none
        << ",\"unit_tolerance\":" << json_number(unit_weight_tolerance)
        << ",\"max_unit_deviation\":" << json_number(evaluation.max_unit_deviation)
        << ",\"active_influence_histogram\":[";
    for (std::size_t index = 0; index < evaluation.active_histogram.size(); ++index)
        out << (index ? "," : "") << evaluation.active_histogram[index];
    out << "],\"normalized\":false}"
        << ",\"predicted_vertex_count\":" << evaluation.predicted
        << ",\"unpredicted_vertex_count\":" << (evaluation.vertices.size() - evaluation.predicted)
        << ",\"change_epsilon\":" << json_number(change_epsilon)
        << ",\"changed_vertex_count\":" << evaluation.changed
        << ",\"max_displacement\":" << json_number(evaluation.max_displacement)
        << ",\"max_displacement_vertex\":"
        << (evaluation.max_displacement_vertex ? std::to_string(*evaluation.max_displacement_vertex) : std::string("null"))
        << ",\"mean_displacement\":" << json_number(evaluation.mean_displacement)
        << ",\"max_bind_residual\":" << json_number(evaluation.max_bind_residual)
        << ",\"bind_bounds\":" << json_bounds(evaluation.bind_bounds)
        << ",\"animated_bounds\":" << json_bounds(evaluation.animated_bounds)
        << ",\"unchanged_scope\":" << json_string(unchanged_scope(evaluation))
        << "}";
}

// `vertices_sha256` is the caller's digest of vertex_block(evaluation).
inline void write_receipt(std::ostream& out, const Header& header, const Evaluation& evaluation,
    const std::string& vertices, const std::string& vertices_sha256) {
    out << "{\n  \"schema\":\"eawr.animation-deformation-probe/1\""
        << ",\n  \"cpu_prediction_only\":true,\"gpu_parity_claim\":false,\"original_parity_claim\":false"
        << ",\"association_promoted\":false,\"space\":\"asset\""
        << ",\n  \"tool\":{\"compiler\":" << json_string(header.compiler) << ",\"config\":" << json_string(header.config)
        << ",\"source_sha256\":{";
    for (std::size_t index = 0; index < header.tool_sources.size(); ++index)
        out << (index ? "," : "") << json_string(header.tool_sources[index].first) << ":"
            << json_string(header.tool_sources[index].second);
    out << "}},\n  \"mounts\":[";
    for (std::size_t index = 0; index < header.mounts.size(); ++index)
        out << (index ? "," : "") << "{\"layer\":" << json_string(header.mounts[index].first)
            << ",\"manifest_source_id\":" << json_string(header.mounts[index].second) << "}";
    out << "],\n  \"model\":";
    write_identity(out, header.model);
    out << ",\n  \"animation\":";
    if (header.animation) write_identity(out, *header.animation);
    else out << "null";
    out << ",\n  \"selection\":{\"mesh\":" << json_string(header.mesh) << ",\"submesh\":" << header.submesh << "}"
        << ",\n  \"sample\":{\"mode\":" << json_string(header.mode)
        << ",\"requested_time_seconds\":" << json_number(header.requested_time)
        << ",\"sampled_time_seconds\":" << json_number(header.sampled_time)
        << ",\"bind_player\":\"separate Player::create(model), clamp t=0\""
        << ",\"animated_player\":" << json_string(header.animation ? "Player::create(model, animation)" : "separate Player::create(model), clamp t=0 (bind_only)")
        << ",\"bone_count\":" << header.bone_count << ",\"track_count\":" << header.track_count
        << ",\"tracked_bones\":[";
    for (std::size_t index = 0; index < header.tracked_bones.size(); ++index)
        out << (index ? "," : "") << header.tracked_bones[index];
    out << "]}"
        << ",\n  \"summary\":";
    write_summary(out, evaluation);
    out << ",\n  \"vertices_sha256\":" << json_string(vertices_sha256)
        << ",\n  \"vertices\":" << vertices << "\n}\n";
}

} // namespace eawr::tests::animation_deformation
