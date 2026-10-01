#pragma once

// #530 (docs/behaviour/space-purchasing.md PU-63 to PU-68): the command bar's station production
// in Godot, besides the build buttons (which take the unit card slots, unit_cards_view.hpp). The
// engine-free presentation::ui production layouts say what to show; this draws it:
//
// - the build queue slots `tqueue00..09`: the queued type's icon (i_button_temporary.tga without
//   one) at its texture's size around the bone, tinted with GameConstants Right_Queue_Tint, and on
//   the front entry of each queue its "<n>%" in the slot's font; a left release on a queued slot
//   cancels that entry (PU-63, PU-64);
// - the credits `Text_Credits_tactical`: the money icon and the whole credits, right-justified
//   (PU-65);
// - the reinforcements button `b_reinforcement`, which opens and closes the reinforcement pane
//   (PU-66, PU-67): the `i_main_reinforce` shell anchored at the top left of the screen, one slot per
//   pooled type with "x<n>", greyed when its population does not fit, the population "<used>/<cap>"
//   and the close button. A left press on an enabled slot starts placing that type (PU-68).

#include "ui/kit.hpp"

#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/production.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

class EawrProductionPanel final : public godot::Control {
    GDCLASS(EawrProductionPanel, godot::Control)

public:
    struct Setup final {
        std::vector<presentation::ui::HudQueueSlot> queue;
        std::optional<presentation::ui::HudIconText> credits;
        std::optional<presentation::ui::HudShellButton> reinforce;
        std::optional<presentation::ui::HudReinforcePane> pane;
        data::ui::Rgba8 queue_tint{255, 128, 128, 215}; // GameConstants Right_Queue_Tint
        std::function<godot::Ref<godot::Texture2D>(const std::string& texture)> texture;
        // The same lookup without a warning when the texture is missing: FoC names the credits
        // icon i_icon_money_big.tga, which no FoC archive holds (PU-65).
        std::function<godot::Ref<godot::Texture2D>(const std::string& texture)> optional_texture;
        std::function<std::string(const std::string& type)> icon; // a type's Icon_Name
        godot::Ref<godot::Font> font;
        std::function<presentation::ui::TextCell(std::int32_t glyph_height)> cell;
        std::function<presentation::ui::ShellPlacement()> placement;
        std::function<presentation::ui::ReferenceSpace()> space;
    };
    struct QueueView final {
        std::size_t component{};
        std::string type;
        double progress{1.0};
        std::optional<std::string> percent;
        bool operator==(const QueueView&) const = default;
    };
    struct PoolView final {
        std::size_t slot{};
        std::string type;
        std::string text;
        bool enabled{};
        bool operator==(const PoolView&) const = default;
    };
    struct View final {
        std::vector<QueueView> queue;
        std::optional<std::string> credits;
        std::vector<PoolView> pool;
        std::string population;
        std::size_t rows{};
        bool reinforcement_allowed{true}; // WR-07/19: permission, command bar, pending victory
        bool operator==(const View&) const = default;
    };

    EawrProductionPanel();
    ~EawrProductionPanel() override;
    void setup(Setup setup);
    void show(View view);
    // A left release on queued component `component` (tqueueNN).
    void set_cancel(std::function<void(std::size_t component)> cancel) { cancel_ = std::move(cancel); }
    // A left press on pool slot `slot` of the open pane.
    void set_pick(std::function<void(std::size_t slot)> pick) { pick_ = std::move(pick); }
    void set_drag(std::function<void(godot::Vector2)> move, std::function<void(godot::Vector2)> drop) {
        drag_move_ = std::move(move);
        drag_drop_ = std::move(drop);
    }
    [[nodiscard]] bool pane_open() const noexcept { return pane_open_; }
    void set_pane_open(bool open);

    // Where scripted hud=<name> gestures point: tqueueNN, b_reinforcement, r_close, r_RRCC.
    [[nodiscard]] std::optional<godot::Rect2> control_rect(const std::string& name) const;
    // The "production" object of the HUD report.
    [[nodiscard]] std::string report_json() const;

    bool _has_point(const godot::Vector2& point) const override;
    void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
    void _draw() override;
    void _notification(int what);

protected:
    static void _bind_methods() {}

private:
    enum class Hit : std::uint8_t { queue, reinforce, close, pool };
    std::function<void(godot::Vector2)> drag_move_;
    std::function<void(godot::Vector2)> drag_drop_;
    bool dragging_pool_{};
    struct Target final {
        Hit kind{Hit::queue};
        std::size_t index{};
    };
    [[nodiscard]] godot::Rect2 screen(const data::ui::ReferenceRect& rect) const;
    [[nodiscard]] godot::Rect2 pane_screen(const data::ui::ReferenceRect& rect) const;
    [[nodiscard]] presentation::ui::ShellPlacement pane_placement() const;
    [[nodiscard]] data::ui::ReferenceRect close_rect() const;
    [[nodiscard]] std::optional<Target> target_at(const godot::Vector2& point) const;
    [[nodiscard]] godot::Ref<godot::Texture2D> icon_texture(const std::string& type, std::string* name = nullptr) const;
    void draw_text(KitText& text, const godot::String& value, const godot::Rect2& box, godot::HorizontalAlignment align,
                   const data::ui::Rgba8& colour, std::int32_t points, bool outline);
    void update_text();

    Setup setup_;
    View view_;
    bool pane_open_{};
    std::function<void(std::size_t)> cancel_;
    std::function<void(std::size_t)> pick_;
    std::optional<Target> pressed_;
    std::vector<std::unique_ptr<KitText>> queue_texts_;
    std::vector<std::unique_ptr<KitText>> pool_texts_;
    KitText credits_text_;
    KitText population_text_;
    godot::Vector2 laid_out_{-1.0F, -1.0F};
    std::uint64_t cancels_{};
    std::uint64_t picks_{};
    std::uint64_t toggles_{};
};

} // namespace eawr::presentation::godot_backend
