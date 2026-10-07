#pragma once

// #530 (docs/behaviour/space-purchasing.md PU-63 to PU-68): the command bar's station production
// in Godot, besides the build buttons (which take the unit card slots, unit_cards_view.hpp). The
// engine-free presentation::ui production layouts say what to show; this draws it:
//
// - the build queue slots `tqueue00..09`: the queued type's icon (i_button_temporary.tga without
//   one) at its texture's size around the bone, tinted with GameConstants Right_Queue_Tint, and on
//   the front entry of each queue its "<n>%" in the slot's font; a right release on a queued slot
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
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/shader_material.hpp>
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
        godot::Ref<godot::Font> close_font;
        std::function<presentation::ui::TextCell(std::int32_t glyph_height)> close_cell;
        std::function<presentation::ui::TextCell(std::int32_t glyph_height)> cell;
        std::function<presentation::ui::ShellPlacement()> placement;
        std::function<presentation::ui::ReferenceSpace()> space;
    };
    struct QueueView final {
        std::size_t component{};
        std::string type;
        double progress{1.0};
        std::optional<std::string> percent;
        std::uint64_t entry_id{};
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
        std::uint64_t notifications{};
        double notification_seconds{};
        bool operator==(const View&) const = default;
    };

    EawrProductionPanel();
    ~EawrProductionPanel() override;
    void setup(Setup setup);
    void show(const View& view, double seconds = 0.0);
    // A right release on queued component `component` (tqueueNN).
    void set_cancel(std::function<void(std::size_t component, std::uint64_t entry_id)> cancel) { cancel_ = std::move(cancel); }
    // A left press on pool slot `slot` of the open pane.
    void set_pick(std::function<void(std::size_t slot)> pick) { pick_ = std::move(pick); }
    void set_open(std::function<void()> open) { open_ = std::move(open); }
    void set_drag(std::function<void(godot::Vector2)> move, std::function<void(godot::Vector2)> drop,
                  std::function<void()> cancel, std::function<void(double)> wheel = {}) {
        drag_move_ = std::move(move);
        drag_drop_ = std::move(drop);
        drag_cancel_ = std::move(cancel);
        drag_wheel_ = std::move(wheel);
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
    std::function<void()> drag_cancel_;
    std::function<void(double)> drag_wheel_;
    bool dragging_pool_{};
    struct Target final {
        Hit kind{Hit::queue};
        std::size_t index{};
        std::uint64_t entry_id{};
    };
    [[nodiscard]] godot::Rect2 screen(const data::ui::ReferenceRect& rect) const;
    [[nodiscard]] godot::Rect2 pane_screen(const data::ui::ReferenceRect& rect) const;
    [[nodiscard]] presentation::ui::ShellPlacement pane_placement() const;
    [[nodiscard]] data::ui::ReferenceRect close_rect() const;
    [[nodiscard]] std::optional<Target> target_at(const godot::Vector2& point) const;
    [[nodiscard]] godot::Ref<godot::Texture2D> icon_texture(const std::string& type, std::string* name = nullptr) const;
    void draw_text(KitText& text, const godot::String& value, const godot::Rect2& box, godot::HorizontalAlignment align,
                   const data::ui::Rgba8& colour, std::int32_t points, bool outline, bool emboss = false,
                   bool close = false);
    void update_text();
    void prepare_art();
    void place_art();
    void show_button(std::size_t layer, bool visible, float level, bool shifted);
    void draw_dial(std::size_t component, double completion);
    [[nodiscard]] bool reinforce_enabled() const noexcept;
    [[nodiscard]] double flash_level() const noexcept;

    Setup setup_;
    View view_;
    bool pane_open_{};
    std::function<void(std::size_t, std::uint64_t)> cancel_;
    std::function<void(std::size_t)> pick_;
    std::function<void()> open_;
    std::optional<Target> pressed_;
    bool pressed_right_{};
    bool hovered_reinforce_{};
    bool hovered_close_{};
    std::array<godot::Ref<godot::Texture2D>, 3> close_art_;
    std::optional<godot::Rect2> close_art_rect_;
    double seconds_{};
    std::optional<double> flash_start_;
    double pressed_seconds_{-1.0};
    godot::RID button_item_;
    godot::RID additive_item_;
    godot::Ref<godot::Material> additive_material_;
    struct ButtonArt {
        godot::RID item;
        godot::Ref<godot::Texture2D> texture;
    };
    std::array<ButtonArt, 6> button_art_;
    struct DialArt {
        godot::RID item;
        godot::Ref<godot::Texture2D> texture;
        godot::Ref<godot::ShaderMaterial> material;
    };
    std::vector<DialArt> dial_art_;
    std::vector<godot::Ref<godot::Texture2D>> queue_icons_;
    godot::Ref<godot::ArrayMesh> dial_mesh_;
    godot::StringName completion_parameter_{"completion"};
    std::optional<presentation::ui::ShellPlacement> art_placement_;
    std::uint64_t art_builds_{};
    std::uint64_t art_frames_{};
    std::uint64_t warmed_art_frames_{};
    std::size_t art_allocations_{};
    std::size_t art_buffer_work_{};
    bool allocation_probe_{};
    bool reinforce_drawn_{};
    bool flash_drawn_{};
    std::uint64_t flashes_started_{};
    std::uint64_t flash_frames_{};
    std::uint32_t dials_drawn_{};
    std::vector<std::unique_ptr<KitText>> queue_texts_;
    std::vector<std::unique_ptr<KitText>> pool_texts_;
    KitText credits_text_;
    KitText population_text_;
    KitText close_text_;
    godot::Vector2 laid_out_{-1.0F, -1.0F};
    std::uint64_t cancels_{};
    std::uint64_t picks_{};
    std::uint64_t toggles_{};
};

} // namespace eawr::presentation::godot_backend
