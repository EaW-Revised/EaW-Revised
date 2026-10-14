#pragma once

#include "battle_content.hpp"
#include "eawr/platform/campaign_loading_view.hpp"
#include <godot_cpp/variant/rect2.hpp>
#include <array>
#include <memory>
#include <optional>
#include <string>

namespace godot { class Control; }
namespace eawr::presentation::godot_backend {

// WGP-32/51/52: a private loading scene, never a resident galaxy capture.
class LoadingPlanet final {
public:
    LoadingPlanet();
    ~LoadingPlanet();
    bool build(godot::Control& parent, const godot::Rect2& rect, const BattleContent& content,
        const std::string& location, const platform::CampaignLoadingDescriptor* descriptor,
        const std::optional<std::string>& current_model, const std::optional<std::array<float, 16>>& current_transform);
    void advance(double seconds);
    void release();
    [[nodiscard]] std::string report_json() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
