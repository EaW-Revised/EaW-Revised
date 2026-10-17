#pragma once
#include "ground_roster.hpp"
#include "fleet_drag.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/texture_button.hpp>
#include <godot_cpp/classes/label.hpp>
#include <memory>
#include <map>

namespace eawr::presentation::godot_backend {
// GUI-owned ground selection lifetime. WGUI-52's box threshold is separate
// from the unqualified widget-drag activation binding (WGUI-U04).
class EawrGroundInput final : public godot::Control {
    GDCLASS(EawrGroundInput, godot::Control)
public:
    EawrGroundInput();
    std::function<bool(godot::Vector2)> hit;
    std::function<void(int, godot::Vector2, bool, bool)> event;
    std::function<bool(godot::Vector2, bool)> right_event;
    std::function<std::optional<godot::Rect2>()> rectangle;
    EawrFleetDrag* drag_art{};
    bool _has_point(const godot::Vector2& point) const override;
    void _gui_input(const godot::Ref<godot::InputEvent>& input) override;
    void _notification(int what);
protected:
    static void _bind_methods() {}
};
struct GroundHudState {
    std::map<std::string, godot::TextureButton*> buttons;
    std::vector<viewer::galactic::InputWidget> widgets;
    std::map<viewer::galactic::ObjectId, viewer::galactic::GroundRoster> rosters;
    // #3931: launch/land gating reads rosters built from the truth publication; equals rosters when the reveal is off.
    std::map<viewer::galactic::ObjectId, viewer::galactic::GroundRoster> command_rosters;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> publication;
    EawrGroundInput* input{};
    godot::Label* count{};
    godot::TextureButton* sale{};
    godot::Label* sale_label{};
    viewer::galactic::ObjectId sale_object{}, sale_planet{};
    godot::Vector2 sale_centre;
    std::string sale_text, busy_text;
    std::uint64_t sale_opened{}, sales_submitted{};
    viewer::galactic::ObjectId planet{}, pressed{}, pressed_object{};
    bool control{}, double_release{};
    std::uint64_t rebuilds{}, clicks{}, boxes{}, land_orders{}, rejected{};
    // #2880: the last accepted WGM-13 launch drop (seat command 18), for reports and receipts.
    std::uint64_t launch_orders{};
    // GHUD-S69: the pad a company was last dropped on, per planet. Presentation only: retail writes a
    // per-object GUI index through its event queue; here it is viewer state, never replayed or saved.
    std::map<viewer::galactic::ObjectId, std::map<viewer::galactic::ObjectId, std::size_t>> pad_slots;
    std::uint64_t reorders{};
    bool drag_sound{}, layout_dirty{};
    std::vector<viewer::galactic::ObjectId> last_launch_units;
    std::vector<viewer::galactic::ObjectId> last_launch_path;
    viewer::galactic::ObjectId last_launch_target{};
    std::uint8_t last_launch_slot{};
};
} // namespace eawr::presentation::godot_backend
