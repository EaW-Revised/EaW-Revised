#pragma once

#include "eawr/presentation/ui/input_routing.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/viewport.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

// UI-07 (#304, docs/ui/ui-layer.md §3.3): the Godot half of input routing. The GUI takes
// events first (modal layer, then HUD controls); the host's _unhandled_input hands the rest to
// the world through the policy in eawr/presentation/ui/input_routing.hpp.
namespace eawr::presentation::godot_backend {

// A full-viewport HUD root that stops the pointer only where `hit_test` says, in viewport
// pixels: over a component rect or an opaque faceplate texel (UI-I2, HudViewModel::
// hit_test_screen). Anywhere else the pointer falls through to the world. Child controls
// (buttons on component rects) are picked before this root, as usual in Godot.
class EawrUiHitMask final : public godot::Control {
    GDCLASS(EawrUiHitMask, godot::Control)

public:
    using HitTest = std::function<bool(double x, double y)>;

    EawrUiHitMask();
    void set_hit_test(HitTest hit_test) { hit_test_ = std::move(hit_test); }
    bool _has_point(const godot::Vector2& point) const override;

protected:
    static void _bind_methods() {}

private:
    HitTest hit_test_;
};

// A modal dialog's backdrop: covers the viewport and stops every pointer event. While one is
// visible in the tree, keys never reach the world either (the policy's modal_open).
class EawrUiModalLayer final : public godot::Control {
    GDCLASS(EawrUiModalLayer, godot::Control)

public:
    static constexpr const char* group = "eawr_ui_modal";
    EawrUiModalLayer();

protected:
    static void _bind_methods() {}
};

// The policy class of a Godot event, or nothing for events the world never reads (joypad,
// gestures, actions). `button` is the mouse button index for presses and releases.
[[nodiscard]] std::optional<presentation::ui::InputClass> input_class(
    const godot::Ref<godot::InputEvent>& event, std::uint32_t& button);

// Whether `control` takes typed text (LineEdit, TextEdit and their subclasses such as the
// kit's edit box).
[[nodiscard]] bool is_text_control(const godot::Control* control);

// The viewport's focus state: a visible modal layer and a text control with keyboard focus.
[[nodiscard]] presentation::ui::InputFocus input_focus(godot::Viewport& viewport);

// Registers the routing classes with ClassDB (scene initialisation level).
void register_ui_input_classes();

} // namespace eawr::presentation::godot_backend
