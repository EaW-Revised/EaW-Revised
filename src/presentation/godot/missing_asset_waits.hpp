#pragma once

#include "eawr/sim/snapshot.hpp"

#include <cstddef>
#include <unordered_map>

namespace eawr::presentation::godot_backend::detail {

// Entities whose snapshot asset is not uploaded. One submit renews the waits
// it still observes; a wait is reported when it starts or its asset changes,
// not on every frame, so one unchanged wait held across submits cannot flood
// the bounded diagnostic history and evict unrelated failures. A wait not
// renewed by the next submit (the entity recovered or left the scene) ends,
// and a later return reports again: an entity that keeps leaving and
// returning to a missing asset still reports once per return.
class MissingAssetWaits final {
public:
    void begin() { renewed_.clear(); }

    // True when this wait must be reported.
    [[nodiscard]] bool wait(const sim::EntityId entity, const sim::AssetId asset) {
        renewed_.insert_or_assign(entity, asset);
        const auto previous = current_.find(entity);
        return previous == current_.end() || previous->second != asset;
    }

    void end() { current_.swap(renewed_); }

    [[nodiscard]] std::size_t size() const noexcept { return current_.size(); }

private:
    std::unordered_map<sim::EntityId, sim::AssetId> current_;
    std::unordered_map<sim::EntityId, sim::AssetId> renewed_;
};

} // namespace eawr::presentation::godot_backend::detail
