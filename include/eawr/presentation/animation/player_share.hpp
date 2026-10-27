#pragma once

#include "eawr/presentation/animation/animation.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>

namespace eawr::presentation::animation {

// pf-mem-9: Players are immutable after create() and hold no per-instance state, so every user of one model's
// bind skeleton can share one. Keyed by the caller's model identity (its logical path). Not thread-safe: the
// owner confines it to one thread. It is bounded by the distinct keys inserted; clear() releases its references
// (bindings that already hold a shared_ptr keep theirs).
class PlayerShare final {
public:
    // The shared bind-pose Player for `key`, created from `model` on first use; nullptr when create() fails
    // (failures are not cached).
    [[nodiscard]] std::shared_ptr<const Player> bind(const std::string& key, const assets::Model& model) {
        const auto found = players_.find(key);
        if (found != players_.end()) return found->second;
        auto created = Player::create(model);
        if (!created) return nullptr;
        return players_.emplace(key, std::make_shared<const Player>(std::move(created.value()))).first->second;
    }
    void clear() noexcept { players_.clear(); }
    [[nodiscard]] std::size_t size() const noexcept { return players_.size(); }

private:
    std::map<std::string, std::shared_ptr<const Player>> players_;
};

} // namespace eawr::presentation::animation
