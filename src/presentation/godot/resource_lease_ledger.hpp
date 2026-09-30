#pragma once

#include "eawr/presentation/renderer.hpp"

#include <cstddef>
#include <map>
#include <vector>

namespace eawr::presentation::godot_backend::detail {

// Stable asset IDs are the ownership key; Godot RIDs remain implementation
// details in renderer.cpp. Keeping this small ledger engine-free makes churn
// transitions directly testable without leaking Godot types across the public
// presentation boundary.
class ResourceLeaseLedger final {
public:
    enum class Release { missing, retained, free };

    [[nodiscard]] bool upload(const sim::AssetId asset_id) {
        return leases_.emplace(asset_id, 1).second;
    }

    [[nodiscard]] bool retain(const sim::AssetId asset_id) {
        const auto found = leases_.find(asset_id);
        if (found == leases_.end()) return false;
        ++found->second;
        return true;
    }

    [[nodiscard]] Release release(const sim::AssetId asset_id) {
        const auto found = leases_.find(asset_id);
        if (found == leases_.end()) return Release::missing;
        if (--found->second != 0) return Release::retained;
        leases_.erase(found);
        return Release::free;
    }

    [[nodiscard]] bool contains(const sim::AssetId asset_id) const {
        return leases_.contains(asset_id);
    }

    [[nodiscard]] std::size_t references(const sim::AssetId asset_id) const {
        const auto found = leases_.find(asset_id);
        return found == leases_.end() ? 0 : found->second;
    }

    [[nodiscard]] std::vector<ResourceReference> resources() const {
        std::vector<ResourceReference> result;
        result.reserve(leases_.size());
        for (const auto& [asset_id, references] : leases_) result.push_back({asset_id, references});
        return result;
    }

private:
    std::map<sim::AssetId, std::size_t> leases_;
};

} // namespace eawr::presentation::godot_backend::detail
