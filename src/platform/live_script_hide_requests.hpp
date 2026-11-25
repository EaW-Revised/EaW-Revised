#pragma once
#include "eawr/core/text.hpp"
#include "eawr/platform/live_session.hpp"
#include <map>
#include <string>
#include <utility>
#include <vector>
namespace eawr::platform {
// The final update per mesh is enough between viewer drains. Sound follows the last call independently.
class LiveScriptHideRequests {
public:
    void add(LiveSubObjectHideRequest request) {
        sound_.insert_or_assign(request.entity, request.hidden);
        if (request.sub_object) {
            auto key = std::make_pair(request.entity, core::to_lower(request.name));
            meshes_.insert_or_assign(std::move(key), std::move(request));
        }
    }
    [[nodiscard]] std::vector<LiveSubObjectHideRequest> take() {
        std::vector<LiveSubObjectHideRequest> result;
        result.reserve(meshes_.size() + sound_.size());
        for (auto& [key, request] : meshes_) {
            static_cast<void>(key);
            result.push_back(std::move(request));
        }
        for (const auto& [entity, hidden] : sound_) result.push_back({entity, hidden, {}, false});
        meshes_.clear();
        sound_.clear();
        return result;
    }
    void clear() { meshes_.clear(); sound_.clear(); }
private:
    std::map<std::pair<sim::EntityId, std::string>, LiveSubObjectHideRequest> meshes_;
    std::map<sim::EntityId, bool> sound_;
};
} // namespace eawr::platform
