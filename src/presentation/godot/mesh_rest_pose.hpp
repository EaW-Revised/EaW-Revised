#pragma once

#include "eawr/presentation/animation/animation.hpp"

namespace eawr::presentation::godot_backend::detail {

// GM-05: validate the rigid bind pose before choosing eager or dormant upload.
inline bool prepare_rigid_rest_pose(const assets::Model& source, const bool rigid,
    std::optional<animation::Pose>& rest) {
    if (!rigid || rest) return true;
    auto player = animation::Player::create(source);
    if (!player) return false;
    auto sampled = player.value().sample({});
    if (!sampled) return false;
    rest = std::move(sampled.value());
    return true;
}

} // namespace eawr::presentation::godot_backend::detail
