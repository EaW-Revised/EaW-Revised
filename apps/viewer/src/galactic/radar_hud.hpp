#pragma once

#include "map_input.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <functional>

namespace eawr::presentation::godot_backend {
// Main-viewport GUI admission owns every pointer event on the radar; accepted
// clicks call the existing map-input/camera intent, never a simulation object.
class EawrGalacticRadar final : public godot::Control {
    GDCLASS(EawrGalacticRadar, godot::Control)
public:
    EawrGalacticRadar();
    using Click = std::function<bool(eawr::viewer::galactic::PointerPoint, float, float)>;
    void set_actions(Click click, std::function<void(bool)> wheel, std::function<void(bool)> release);
    void set_enabled(bool enabled) { enabled_ = enabled; }
    void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
protected:
    static void _bind_methods() {}
private:
    Click click_;
    std::function<void(bool)> wheel_;
    std::function<void(bool)> release_;
    bool enabled_{true};
};
} // namespace eawr::presentation::godot_backend
