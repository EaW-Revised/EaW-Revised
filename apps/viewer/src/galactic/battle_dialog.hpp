#pragma once
#include "battle_dialog_model.hpp"
#include "pending_battle_hud.hpp"
#include "ui/dialog_builder.hpp"
#include "ui/battle_overlay.hpp"
#include <godot_cpp/classes/label.hpp>
#include <memory>

namespace eawr::presentation::godot_backend {

// Presentation adapter: mounted dialog geometry/skin, conflict-scoped callbacks.
// The caller owns pause lifetime, authoritative choices and summary music.
class BattleDialog final {
public:
    struct Style {
        std::function<BuiltDialog(std::string_view, godot::Vector2)> dialog;
        std::function<BuiltPendingBattleHud(godot::Vector2, bool)> pending;
        std::function<std::string(std::string_view)> text;
        std::function<godot::Ref<godot::Texture2D>(std::string_view)> texture;
    };
    BattleDialog(godot::Control& parent, Style style,
        std::function<bool(std::uint64_t, viewer::galactic::BattleChoice)> choice);
    ~BattleDialog();
    void present(viewer::galactic::BattleDialogView view);
    void service_layout();
    [[nodiscard]] bool visible() const;
    [[nodiscard]] bool blocks_world(godot::Vector2 point) const;
    [[nodiscard]] const viewer::galactic::BattleDialogView& view() const { return view_; }
    [[nodiscard]] std::string report_json() const;
    void scroll(std::size_t side, double value);
    // Keyboard and acceptance drivers share the button's permission/latch.
    void activate(viewer::galactic::BattleChoice choice);
private:
    void rebuild();
    std::vector<BuiltGadget>& gadgets();
    const std::vector<BuiltGadget>& gadgets() const;
    void refresh();
    void toggle_advanced();
    void caption(BuiltGadget& gadget, std::string_view value);
    Style style_;
    std::function<bool(std::uint64_t, viewer::galactic::BattleChoice)> choice_;
    godot::Control* root_{};
    godot::Label* advisor_{};
    std::optional<BuiltDialog> dialog_;
    std::optional<BuiltPendingBattleHud> pending_;
    std::vector<EawrOverlayButton*> relays_;
    viewer::galactic::BattleDialogView view_;
    std::array<std::size_t, 2> scroll_{};
    std::array<std::array<EawrUiLabel*, 5>, 2> hero_counts_{};
    godot::Vector2 size_;
    double listed_at_{-1};
    bool advanced_{true}, submitted_{};
    bool summary_rows_dirty_{true};
    std::uint64_t summary_refreshes_{};
    std::uint64_t choices_{}, rebuilds_{};
};
}
