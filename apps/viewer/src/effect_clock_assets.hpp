#pragma once

#include "eawr/presentation/renderer.hpp"

#include <algorithm>
#include <span>
#include <unordered_set>
#include <vector>

namespace eawr::presentation::godot_backend {

// Live pieces and fog memories can lease the same material. Clock ownership
// ends only when the renderer frees the resource after its final lease.
inline void prune_effect_clock_assets(std::vector<sim::AssetId>& clocks,
                                     const std::span<const ResourceReference> resources) {
    std::unordered_set<sim::AssetId> alive;
    alive.reserve(resources.size());
    for (const auto& resource : resources) {
        if (resource.references != 0) alive.insert(resource.asset_id);
    }
    std::erase_if(clocks, [&](const sim::AssetId asset) { return !alive.contains(asset); });
}

} // namespace eawr::presentation::godot_backend
