#include "eawr/presentation/animation/shot_readiness.hpp"

#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace eawr::presentation::animation {
namespace {

[[nodiscard]] core::Diagnostic failure(const std::string_view code, std::string message, const std::string_view path = {}) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = core::Severity::error;
    diagnostic.message = std::move(message);
    if (!path.empty()) diagnostic.logical_path = std::string(path);
    return diagnostic;
}

template <typename T>
[[nodiscard]] core::Result<T> refuse(const std::string_view code, std::string message, const std::string_view path = {}) {
    return core::Result<T>::failure(failure(code, std::move(message), path));
}

// Accumulates bytes for a digest with a fixed, platform-independent encoding.
class DigestInput final {
public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }
    void u64(const std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void f32(const float value) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (int shift = 0; shift < 32; shift += 8) bytes_.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
    void text(const std::string_view value) {
        u64(value.size());
        for (const char c : value) bytes_.push_back(static_cast<std::uint8_t>(c));
    }
    void matrix(const Matrix& value) { for (const float element : value) f32(element); }
    [[nodiscard]] std::string hex() const { return core::sha256_hex(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

[[nodiscard]] core::Result<std::size_t> find_mesh(const assets::Model& model, const std::string_view name) {
    std::size_t found = model.meshes.size();
    for (std::size_t index = 0; index < model.meshes.size(); ++index) {
        if (model.meshes[index].name != name) continue;
        if (found != model.meshes.size())
            return refuse<std::size_t>(diagnostic_codes::invalid_request, "mesh name is ambiguous: " + std::string(name));
        found = index;
    }
    if (found == model.meshes.size())
        return refuse<std::size_t>(diagnostic_codes::invalid_request, "mesh is absent: " + std::string(name));
    if (!model.meshes[found].visible)
        return refuse<std::size_t>(diagnostic_codes::invalid_request, "mesh is not visible: " + std::string(name));
    return core::Result<std::size_t>::success(found);
}

[[nodiscard]] bool active(const float weight) noexcept { return weight != 0.0F; }

} // namespace

core::Result<TrackedBindCensus> tracked_bind_census(const assets::Model& model, const assets::Animation* animation) {
    const std::string_view path = model.source.logical_path;
    // The player owns the exact track/bone and hierarchy contract.
    auto player = Player::create(model, animation);
    if (!player) return core::Result<TrackedBindCensus>::failure(player.error());

    TrackedBindCensus census;
    census.bone_count = model.bones.size();
    census.track_count = animation ? animation->tracks.size() : 0U;
    census.bones.resize(model.bones.size());
    for (std::size_t index = 0; index < model.bones.size(); ++index) {
        census.bones[index].name = model.bones[index].name;
        census.bones[index].parent = model.bones[index].parent;
    }
    if (animation) for (const auto& track : animation->tracks) census.bones[track.bone_index].tracked = true;
    // Parents precede children (Player::create enforced it), so one pass suffices.
    for (auto& bone : census.bones) {
        if (bone.tracked || bone.parent < 0) continue;
        const auto& parent = census.bones[static_cast<std::size_t>(bone.parent)];
        bone.inherits_tracked = parent.tracked || parent.inherits_tracked;
    }

    const std::size_t bone_count = model.bones.size();
    for (const auto& mesh : model.meshes) {
        if (mesh.bone < -1)
            return refuse<TrackedBindCensus>(diagnostic_codes::invalid_model, "mesh bone is below -1: " + mesh.name, path);
        for (const auto& submesh : mesh.submeshes) {
            if (submesh.skin_bones.empty()) continue;
            if (bone_count == 0)
                return refuse<TrackedBindCensus>(diagnostic_codes::invalid_model, "skinned mesh on a boneless model: " + mesh.name, path);
            for (const std::uint32_t global : submesh.skin_bones) if (global >= bone_count)
                return refuse<TrackedBindCensus>(diagnostic_codes::invalid_model, "palette bone is outside the model: " + mesh.name, path);
        }
        if (mesh.bone >= 0 && static_cast<std::size_t>(mesh.bone) >= bone_count) {
            bool any_rigid = false;
            for (const auto& submesh : mesh.submeshes) any_rigid = any_rigid || submesh.skin_bones.empty();
            if (any_rigid)
                return refuse<TrackedBindCensus>(diagnostic_codes::invalid_model, "rigid mesh bone is outside the model: " + mesh.name, path);
        }
        bool rigid_counted = false;
        for (const auto& submesh : mesh.submeshes) {
            if (submesh.skin_bones.empty()) {
                if (mesh.bone < 0 || rigid_counted) continue;
                rigid_counted = true;
                auto& bone = census.bones[static_cast<std::size_t>(mesh.bone)];
                if (mesh.visible) ++bone.rigid_meshes; else ++bone.hidden_bindings;
                continue;
            }
            std::vector<bool> weighted(submesh.skin_bones.size(), false);
            for (const auto& vertex : submesh.vertices) for (std::size_t slot = 0; slot < 4; ++slot) {
                const float weight = vertex.bone_weights[slot];
                if (!std::isfinite(weight) || weight < 0.0F)
                    return refuse<TrackedBindCensus>(diagnostic_codes::invalid_model, "negative or non-finite skin weight: " + mesh.name, path);
                if (!active(weight)) continue;
                const std::uint32_t local = vertex.bone_indices[slot];
                if (local >= submesh.skin_bones.size())
                    return refuse<TrackedBindCensus>(diagnostic_codes::invalid_model, "active palette index is outside the palette: " + mesh.name, path);
                weighted[local] = true;
            }
            // A bone listed twice in one palette still counts once per submesh.
            std::vector<std::uint32_t> listed(submesh.skin_bones.begin(), submesh.skin_bones.end());
            std::vector<std::uint32_t> used;
            for (std::size_t slot = 0; slot < weighted.size(); ++slot) if (weighted[slot]) used.push_back(submesh.skin_bones[slot]);
            std::sort(listed.begin(), listed.end()); listed.erase(std::unique(listed.begin(), listed.end()), listed.end());
            std::sort(used.begin(), used.end()); used.erase(std::unique(used.begin(), used.end()), used.end());
            for (const std::uint32_t global : listed) if (mesh.visible) ++census.bones[global].palette_listed;
            for (const std::uint32_t global : used) {
                if (mesh.visible) ++census.bones[global].palette_weighted; else ++census.bones[global].hidden_bindings;
            }
        }
    }
    for (const auto& proxy : model.proxies) {
        if (proxy.bone >= bone_count)
            return refuse<TrackedBindCensus>(diagnostic_codes::invalid_model, "proxy bone is outside the model: " + proxy.name, path);
        ++census.bones[proxy.bone].proxies;
    }
    for (const auto& light : model.lights) {
        if (light.bone < -1 || (light.bone >= 0 && static_cast<std::size_t>(light.bone) >= bone_count))
            return refuse<TrackedBindCensus>(diagnostic_codes::invalid_model, "light bone is outside the model: " + light.name, path);
        if (light.bone >= 0) ++census.bones[static_cast<std::size_t>(light.bone)].lights;
    }
    for (const auto& dazzle : model.dazzles) {
        if (dazzle.bone >= bone_count)
            return refuse<TrackedBindCensus>(diagnostic_codes::invalid_model, "dazzle bone is outside the model: " + dazzle.name, path);
        ++census.bones[dazzle.bone].dazzles;
    }
    for (const auto& bone : census.bones) {
        if (bone.draw_bound()) {
            ++census.draw_bound;
            if (bone.tracked) ++census.draw_bound_tracked;
            else if (bone.inherits_tracked) ++census.draw_bound_inherited;
            else ++census.draw_bound_static;
        } else if (bone.tracked) {
            ++census.tracked_not_draw_bound;
        }
        if (bone.palette_listed != 0 && bone.palette_weighted == 0) ++census.palette_listed_unweighted;
    }
    return core::Result<TrackedBindCensus>::success(std::move(census));
}

core::Result<SubmeshPalette> submesh_palette(const Player& player, const Pose& pose, const assets::Model& model,
    const TrackedBindCensus& census, const std::string_view mesh_name, const std::size_t submesh_index) {
    if (!player.sampled(pose))
        return refuse<SubmeshPalette>(diagnostic_codes::invalid_request, "pose was not sampled by this player");
    if (pose.bones.size() != model.bones.size() || player.bone_count() != model.bones.size()
        || census.bones.size() != model.bones.size())
        return refuse<SubmeshPalette>(diagnostic_codes::invalid_request, "pose, player, census and model bone counts differ");
    for (std::size_t index = 0; index < model.bones.size(); ++index) if (census.bones[index].name != model.bones[index].name)
        return refuse<SubmeshPalette>(diagnostic_codes::invalid_request, "census does not describe this model");
    const auto found = find_mesh(model, mesh_name);
    if (!found) return core::Result<SubmeshPalette>::failure(found.error());
    const auto& mesh = model.meshes[found.value()];
    if (submesh_index >= mesh.submeshes.size())
        return refuse<SubmeshPalette>(diagnostic_codes::invalid_request, "submesh is out of range: " + mesh.name);
    const auto& submesh = mesh.submeshes[submesh_index];

    SubmeshPalette result;
    result.mesh = mesh.name;
    result.submesh = submesh_index;
    result.shader = submesh.shader;
    result.vertex_count = submesh.vertices.size();
    const auto slot_for = [&](const std::uint32_t local, const std::uint32_t global) {
        PaletteSlot slot;
        slot.local = local; slot.bone = global; slot.name = model.bones[global].name;
        slot.tracked = census.bones[global].tracked; slot.inherits_tracked = census.bones[global].inherits_tracked;
        slot.skin_asset = pose.bones[global].skin_asset;
        return slot;
    };
    if (!submesh.skin_bones.empty()) {
        result.route = SkinRoute::palette;
        for (std::uint32_t local = 0; local < submesh.skin_bones.size(); ++local) {
            const std::uint32_t global = submesh.skin_bones[local];
            if (global >= model.bones.size())
                return refuse<SubmeshPalette>(diagnostic_codes::invalid_model, "palette bone is outside the model: " + mesh.name);
            result.slots.push_back(slot_for(local, global));
        }
        for (const auto& vertex : submesh.vertices) for (std::size_t slot = 0; slot < 4; ++slot) {
            const float weight = vertex.bone_weights[slot];
            if (!std::isfinite(weight) || weight < 0.0F)
                return refuse<SubmeshPalette>(diagnostic_codes::invalid_model, "negative or non-finite skin weight: " + mesh.name);
            if (!active(weight)) continue;
            const std::uint32_t local = vertex.bone_indices[slot];
            if (local >= result.slots.size())
                return refuse<SubmeshPalette>(diagnostic_codes::invalid_model, "active palette index is outside the palette: " + mesh.name);
            ++result.slots[local].active_influences;
        }
    } else if (mesh.bone >= 0) {
        if (static_cast<std::size_t>(mesh.bone) >= model.bones.size())
            return refuse<SubmeshPalette>(diagnostic_codes::invalid_model, "rigid mesh bone is outside the model: " + mesh.name);
        result.route = SkinRoute::rigid;
        result.slots.push_back(slot_for(0U, static_cast<std::uint32_t>(mesh.bone)));
        result.slots.back().active_influences = submesh.vertices.size();
    } else if (mesh.bone == -1) {
        result.route = SkinRoute::unskinned;
    } else {
        return refuse<SubmeshPalette>(diagnostic_codes::invalid_model, "mesh bone is below -1: " + mesh.name);
    }
    for (const auto& slot : result.slots) for (const float element : slot.skin_asset) if (!std::isfinite(element))
        return refuse<SubmeshPalette>(diagnostic_codes::invalid_request, "palette skin matrix is non-finite: " + slot.name);
    return core::Result<SubmeshPalette>::success(std::move(result));
}

core::Result<AttachmentProbe> attachment_probe(const Player& player, const Pose& reference, const Pose& sampled,
    const TrackedBindCensus& census, const std::string_view bone) {
    if (census.bones.size() != player.bone_count())
        return refuse<AttachmentProbe>(diagnostic_codes::invalid_request, "census does not describe this player");
    auto first = player.attachment(reference, bone, AttachmentSpace::model);
    if (!first) return core::Result<AttachmentProbe>::failure(first.error());
    auto second = player.attachment(sampled, bone, AttachmentSpace::model);
    if (!second) return core::Result<AttachmentProbe>::failure(second.error());
    // attachment() already proved the name is present exactly once.
    std::size_t index = 0;
    while (census.bones[index].name != bone) ++index;
    AttachmentProbe result;
    result.bone = std::string(bone);
    result.index = index;
    result.tracked = census.bones[index].tracked;
    result.inherits_tracked = census.bones[index].inherits_tracked;
    result.reference = first.value();
    result.sampled = second.value();
    const auto& a = result.reference.column_major;
    const auto& b = result.sampled.column_major;
    double squared = 0.0;
    for (std::size_t row = 0; row < 3; ++row) {
        const double difference = static_cast<double>(b[12 + row]) - static_cast<double>(a[12 + row]);
        squared += difference * difference;
    }
    result.translation_delta = std::sqrt(squared);
    for (std::size_t column = 0; column < 3; ++column) for (std::size_t row = 0; row < 3; ++row)
        result.basis_delta = std::max(result.basis_delta,
            std::abs(static_cast<double>(b[column * 4 + row]) - static_cast<double>(a[column * 4 + row])));
    if (!std::isfinite(result.translation_delta) || !std::isfinite(result.basis_delta))
        return refuse<AttachmentProbe>(diagnostic_codes::invalid_request, "attachment difference is non-finite");
    return core::Result<AttachmentProbe>::success(std::move(result));
}

core::Result<std::vector<std::size_t>> moved_bones(
    const Player& player, const Pose& reference, const Pose& sampled, const float epsilon) {
    if (!player.sampled(reference) || !player.sampled(sampled))
        return refuse<std::vector<std::size_t>>(diagnostic_codes::invalid_request, "pose was not sampled by this player");
    if (!std::isfinite(epsilon) || epsilon < 0.0F)
        return refuse<std::vector<std::size_t>>(diagnostic_codes::invalid_request, "epsilon must be finite and non-negative");
    if (reference.bones.size() != sampled.bones.size() || reference.bones.size() != player.bone_count())
        return refuse<std::vector<std::size_t>>(diagnostic_codes::invalid_request, "pose bone counts differ");
    std::vector<std::size_t> moved;
    for (std::size_t index = 0; index < reference.bones.size(); ++index) {
        const auto& a = reference.bones[index];
        const auto& b = sampled.bones[index];
        bool differs = false;
        for (std::size_t element = 0; element < 16 && !differs; ++element) {
            const float model_delta = std::abs(b.model_asset[element] - a.model_asset[element]);
            const float skin_delta = std::abs(b.skin_asset[element] - a.skin_asset[element]);
            if (!std::isfinite(model_delta) || !std::isfinite(skin_delta))
                return refuse<std::vector<std::size_t>>(diagnostic_codes::invalid_request, "pose matrix difference is non-finite");
            differs = model_delta > epsilon || skin_delta > epsilon;
        }
        if (differs) moved.push_back(index);
    }
    return core::Result<std::vector<std::size_t>>::success(std::move(moved));
}

std::string pose_digest(const Pose& pose) {
    DigestInput input;
    input.text("eawr.pose-digest.v1");
    input.u64(pose.bones.size());
    for (const auto& bone : pose.bones) {
        input.u8(bone.visible ? 1U : 0U);
        input.matrix(bone.local_asset);
        input.matrix(bone.model_asset);
        input.matrix(bone.skin_asset);
    }
    return input.hex();
}

std::string palette_digest(const SubmeshPalette& palette) {
    DigestInput input;
    input.text("eawr.palette-digest.v1");
    input.text(to_string(palette.route));
    input.text(palette.mesh);
    input.u64(palette.submesh);
    input.u64(palette.slots.size());
    for (const auto& slot : palette.slots) {
        input.u64(slot.local);
        input.u64(slot.bone);
        input.text(slot.name);
        input.u64(slot.active_influences);
        input.matrix(slot.skin_asset);
    }
    return input.hex();
}

} // namespace eawr::presentation::animation
