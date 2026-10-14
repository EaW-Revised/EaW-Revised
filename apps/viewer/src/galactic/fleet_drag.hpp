#pragma once

#include <godot_cpp/classes/control.hpp>
#include <functional>
#include <optional>
#include <cstdint>
#include <vector>
#include <utility>

namespace eawr::presentation::godot_backend {
// Native GUI drag lifetime. Activation distance is the project GUI binding,
// not a claimed WGUI-U04 retail threshold. No member-transfer command is made.
// GHUD-S97: source drawing is lifted for the drag lifetime; any retained
// remainder is separate art. Native visibility and hit geometry stay intact.
class EawrFleetDrag final : public godot::Control {
    GDCLASS(EawrFleetDrag, godot::Control)
public:
    using Begin = std::function<std::optional<std::uint64_t>(godot::Vector2)>;
    using Drop = std::function<bool(std::uint64_t, godot::Vector2, bool)>;
    using Hover = std::function<void(godot::Vector2)>;
    EawrFleetDrag();
    void source(Begin begin, std::function<void()> click);
    void right_action(std::function<void(godot::Vector2)> action) { right_ = std::move(action); }
    void receiver(Drop drop, std::function<void()> end, Hover hover);
    // The receiver draws above the HUD; `ghost` (when set) follows the pointer,
    // offset by where the source was grabbed, until the drag ends.
    void start(godot::Control* ghost = nullptr);
    // A copy of `source`'s shown art (its button texture and visible children),
    // placed so that `grab` (source-local) sits on the copy's origin.
    [[nodiscard]] static godot::Control* ghost_of(const godot::Control& source, godot::Vector2 grab,
        std::optional<std::size_t> count = {}, bool base_only = false, bool draw_base = true);
    void lift_source(godot::Control& source, std::optional<std::size_t> remainder = {}, bool base_only = false);
    void cancel();
    bool _has_point(const godot::Vector2& point) const override;
    godot::Variant _get_drag_data(const godot::Vector2& point) override;
    bool _can_drop_data(const godot::Vector2& point, const godot::Variant& data) const override;
    void _drop_data(const godot::Vector2& point, const godot::Variant& data) override;
    void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
    void _notification(int what);
protected:
    static void _bind_methods() {}
private:
    std::function<void(godot::Vector2)> right_;
    Begin begin_;
    Drop drop_;
    Hover hover_;
    std::function<void()> click_, end_;
    godot::Control* ghost_{};
    // Weak identities allow a publication or scene teardown to remove a source.
    std::vector<std::pair<std::uint64_t, godot::Control*>> lifted_;
    godot::Vector2 pointer_;
    bool receiver_{}, active_{}, double_release_{}, dragged_{};
};
} // namespace eawr::presentation::godot_backend
