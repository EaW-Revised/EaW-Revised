#pragma once
#include "battle_camera.hpp"
#include "map_camera.hpp"
#include "eawr/presentation/renderer.hpp"

namespace eawr::presentation::godot_backend {

class LandBattleCamera final : public BattleCamera {
public:
    LandBattleCamera(const FixedCamera& frame, viewer::MapCameraBridge* bridge) : frame_(frame), bridge_(bridge) {}
    std::optional<camera::TacticalFrame> live_camera_frame() const override {
        return camera::TacticalFrame{frame_.width, frame_.height, frame_.vertical_fov_degrees,
            frame_.near_plane, frame_.far_plane, frame_.eye, frame_.target, frame_.up};
    }
    std::optional<camera::SourceTargetBounds> live_camera_bounds() const override {
        return bridge_ ? std::optional(bridge_->config().bounds) : std::nullopt;
    }
    std::pair<bool, bool> live_camera_pointer_mode(bool ctrl) const override {
        if (!bridge_ || !bridge_->active()) return {};
        const auto& adapter = bridge_->adapter();
        const bool grab = adapter.is_held(viewer::camera_input::Action::rotate_grab);
        return {adapter.is_held(viewer::camera_input::Action::push_scroll) || (grab && !ctrl), grab && ctrl};
    }
    void live_camera_focus(float x, float y) override { if (bridge_) (void)bridge_->focus(x, -y); }
    void live_camera_overview_key() override {}
    std::string live_camera_overview() const override { return "off"; }
private:
    const FixedCamera& frame_;
    viewer::MapCameraBridge* bridge_{};
};

} // namespace eawr::presentation::godot_backend
