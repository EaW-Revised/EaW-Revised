#pragma once

#include "eawr/presentation/animation/animation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace eawr::presentation {

// Upload validation records the global palette indices with positive vertex
// weights. The full pose remains cached for attachments and billboard posing.
[[nodiscard]] inline bool mark_skin_bone_usage(const std::span<std::uint8_t> used,
    const std::uint32_t bone, const float weight) {
    if (!std::isfinite(weight) || weight < 0.0F) return false;
    if (weight == 0.0F) return true;
    if (bone >= used.size()) return false;
    used[bone] = 1;
    return true;
}

[[nodiscard]] inline bool filter_skin_palette_changes(const std::span<std::uint8_t> changes,
    const std::span<const std::uint8_t> used) {
    if (changes.size() != used.size()) return false;
    for (std::size_t bone = 0; bone < changes.size(); ++bone) changes[bone] = changes[bone] && used[bone];
    return true;
}

// Validate before touching the retained buffers: a rejected pose must not
// replace any part of the previous palette. Conversion is linear in bones.
inline bool valid_skin_pose(const std::span<const animation::BonePose> bones) {
    return std::all_of(bones.begin(), bones.end(), [](const animation::BonePose& bone) {
        const auto finite = [](const float value) { return std::isfinite(value); };
        return std::all_of(bone.skin_asset.begin(), bone.skin_asset.end(), finite)
            && std::all_of(bone.model_asset.begin(), bone.model_asset.end(), finite);
    });
}

template <typename Transform, typename Convert>
void store_skin_pose(const std::span<const animation::BonePose> bones,
    std::vector<animation::Matrix>& palette, std::vector<Transform>& model_transforms, Convert&& convert,
    std::vector<std::uint8_t>* changes = nullptr, const bool force_changes = false) {
    const bool replaced = force_changes || palette.size() != bones.size();
    if (changes) changes->assign(bones.size(), 0);
    palette.resize(bones.size());
    model_transforms.resize(bones.size());
    for (std::size_t index = 0; index < bones.size(); ++index) {
        // BP-69: bind and animated bone visibility both reach the rendered mesh.
        // A collapsed basis removes the hidden bone's triangles while retaining
        // the source pose, so a later visible sample restores its full palette.
        auto skin = bones[index].skin_asset;
        auto model = bones[index].model_asset;
        if (!bones[index].visible) {
            std::fill_n(skin.begin(), 12, 0.0F);
            std::fill_n(model.begin(), 12, 0.0F);
        }
        const auto next = animation::Player::asset_to_render_transform(skin);
        if (changes && (replaced || std::bit_cast<std::array<std::uint32_t, 16>>(palette[index])
                != std::bit_cast<std::array<std::uint32_t, 16>>(next))) (*changes)[index] = 1;
        palette[index] = next;
        model_transforms[index] = convert(animation::Player::asset_to_render_transform(model));
    }
}

template <typename Poses, typename Entity, typename Asset, typename Convert>
bool cache_skin_pose(Poses& poses, const Entity entity, const Asset asset,
    const std::span<const animation::BonePose> bones, Convert&& convert,
    std::vector<std::uint8_t>* changes = nullptr) {
    if (!valid_skin_pose(bones)) return false;
    auto& pose = poses[entity];
    const bool replaced = pose.asset_id != asset;
    pose.asset_id = asset;
    store_skin_pose(bones, pose.palette, pose.model_transforms, std::forward<Convert>(convert), changes, replaced);
    return true;
}

// A renderer refreshes billboards serially. Keep scratch at the largest
// uploaded billboard skeleton, and restore it completely for every instance.
template <typename Transform>
struct BillboardPoseScratch final {
    std::vector<Transform> models;
    std::vector<Transform> palettes;

    void prepare(const std::size_t bones) {
        models.reserve(bones);
        palettes.reserve(bones);
    }

    template <typename Convert>
    void reset(const std::span<const Transform> source_models,
        const std::span<const animation::Matrix> source_palette, const Transform& identity, Convert&& convert) {
        models.assign(source_models.begin(), source_models.end());
        palettes.resize(source_models.size());
        for (std::size_t bone = 0; bone < source_models.size(); ++bone)
            palettes[bone] = source_palette.empty() ? identity : convert(source_palette[bone]);
    }
};

// Same parent walk as billboard palette propagation. Counting its steps gives
// a deterministic work bound without changing the order of transform updates.
template <typename Apply>
std::size_t visit_skin_descendants(const std::span<const std::int32_t> parents,
    const std::size_t bone, Apply&& apply) {
    std::size_t steps = 0;
    for (std::size_t child = 0; child < parents.size(); ++child) {
        std::int32_t ancestor = static_cast<std::int32_t>(child);
        while (ancestor >= 0 && ancestor != static_cast<std::int32_t>(bone)) {
            ++steps;
            ancestor = parents[static_cast<std::size_t>(ancestor)];
        }
        if (ancestor == static_cast<std::int32_t>(bone)) apply(child);
    }
    return steps;
}

} // namespace eawr::presentation
