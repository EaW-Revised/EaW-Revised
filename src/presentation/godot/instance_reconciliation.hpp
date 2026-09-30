#pragma once

#include "eawr/sim/snapshot.hpp"

#include <optional>

namespace eawr::presentation::godot_backend::detail {

enum class InstanceTransition {
    retain,
    create,
    replace,
    remove,
};

// A requested asset is absent when it was not uploaded. In that case an old
// instance must be removed rather than left visible with stale resources.
[[nodiscard]] constexpr InstanceTransition reconcile_instance(
    const std::optional<sim::AssetId> current_asset,
    const std::optional<sim::AssetId> requested_asset) noexcept {
    if (!requested_asset) {
        return current_asset ? InstanceTransition::remove : InstanceTransition::retain;
    }
    if (!current_asset) return InstanceTransition::create;
    return *current_asset == *requested_asset
        ? InstanceTransition::retain
        : InstanceTransition::replace;
}

} // namespace eawr::presentation::godot_backend::detail
