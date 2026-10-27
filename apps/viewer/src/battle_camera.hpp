#pragma once

#include "eawr/presentation/camera/controller.hpp"
#include <optional>
#include <string>
#include <utility>

namespace eawr::presentation::godot_backend {

// The drawn camera queried by battle input, independent of the map renderer.
class BattleCamera {
public:
    virtual ~BattleCamera() = default;
    [[nodiscard]] virtual std::optional<camera::TacticalFrame> live_camera_frame() const = 0;
    [[nodiscard]] virtual std::optional<camera::SourceTargetBounds> live_camera_bounds() const = 0;
    [[nodiscard]] virtual std::pair<bool, bool> live_camera_pointer_mode(bool ctrl) const = 0;
    virtual void live_camera_focus(float source_x, float source_y) = 0;
    virtual void live_camera_overview_key() = 0;
    [[nodiscard]] virtual std::string live_camera_overview() const = 0;
};

} // namespace eawr::presentation::godot_backend
