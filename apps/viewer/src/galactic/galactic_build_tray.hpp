#pragma once
#include "galactic_mode.hpp"
#include "planet_hud.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/data/ui/shell_anchors.hpp"
#include "eawr/presentation/ui/build_tray_files.hpp"
#include "eawr/presentation/ui/build_tray_model.hpp"

#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <array>
#include <cstddef>
#include <map>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {
class EawrHudButton;
class EawrHudText;

// The galactic build tray (docs/ui/galactic-build-tray.md, GBT-01 to GBT-03, GBT-06, GBT-10). When the bar
// omits choices, a `+N` handle appears above it; the handle opens a panel that grows upward and lists the
// whole choice list grouped by the model of build_tray_model.hpp. The bar, its slot mapping and the choice
// list are never touched: this only draws on top, and only for a planet whose choices overflow the bar.
// Entries are copies of the bar's own production button, so they take on the mod's look; the panel wears the bar's
// own shell art and colour, and its close button is the mod's own close button (GBT-07, GBT-10).
class GalacticBuildTray final {
public:
    GalacticBuildTray();
    ~GalacticBuildTray();

    // Called every HUD update after the bar has been refreshed.
    void service(GalacticMode::State& state);
    // GBT-14: how far the space and the land queue strips are lifted this frame (pixels), for the report.
    [[nodiscard]] const std::array<float, 2>& queue_lifts() const { return lifts_; }

    // The tray entry under `control`, for the hover and encyclopedia lookup. Valid until the next service().
    [[nodiscard]] const viewer::galactic::BuildChoiceComponent* choice_under(const godot::Control* control) const;

    [[nodiscard]] bool open() const { return open_; }
    // GBT-07: the tray key acts like a click on the handle; it does nothing while no handle is shown (or the HUD is
    // hidden, which hides the handle). `keycode` is the pressed physical key without modifiers. The bound key's name
    // is resolved to a keycode once per binding change; a name the engine does not know falls back to Tab. Returns
    // whether the key was used.
    bool toggle_from_key(std::int64_t keycode);
    [[nodiscard]] bool handle_visible() const;
    [[nodiscard]] std::string report_json() const;

private:
    struct Entry final {
        EawrHudButton* button{};
        EawrHudText* price{};
        viewer::galactic::BuildChoiceComponent choice;
        bool shown{};
    };

    // What the last layout was made from; an unchanged key and choice list leave the controls as they are.
    struct LayoutKey final {
        std::uint64_t planet{};
        bool space_tab{};
        float ui{};
        godot::Vector2 view;
        godot::Rect2 bar, handle;
        friend bool operator==(const LayoutKey&, const LayoutKey&) = default;
    };

    // What the handle was last made from; with an unchanged key and choice list the handle and bar box stay as they are.
    struct HandleKey final {
        std::uint64_t planet{};
        bool space_tab{};
        float ui{};
        godot::Vector2 view;
        std::size_t slots{};
        friend bool operator==(const HandleKey&, const HandleKey&) = default;
    };

    void update(GalacticMode::State& state);
    void lift_queue_strips(GalacticMode::State& state);
    void load_layout(GalacticMode::State& state);
    void report_layout_error();
    void load(GalacticMode::State& state);
    void hide_all();
    void ensure_controls(GalacticMode::State& state);
    void style_handle(GalacticMode::State& state, const data::ui::CommandBarComponent& component,
        const data::ui::ShellAnchor& anchor);
    void place_handle(GalacticMode::State& state, const godot::Rect2& bar);
    void layout_tray(GalacticMode::State& state, const godot::Rect2& bar);
    void style_entry(GalacticMode::State& state, Entry& entry, const viewer::galactic::BuildChoiceComponent& choice,
        const godot::Rect2& rect, float entry_scale);
    // GBT-07: the close button's art, text and place on the open panel.
    void place_close(GalacticMode::State& state);
    // GBT-10: the panel's back and frame, from the bar's shell art under and above `bar` (Back_Source Bar) or from
    // Back_Component.
    void draw_backdrop(GalacticMode::State& state, const godot::Rect2& bar);
    void clear_backdrop();
    [[nodiscard]] float panel_width(GalacticMode::State& state, const godot::Rect2& bar,
        const std::vector<presentation::ui::build_tray::Group>& groups, const presentation::ui::build_tray::Metrics& full,
        const presentation::ui::build_tray::PackOptions& options, float free_height) const;
    [[nodiscard]] bool enabled() const;

    GalacticMode::State* state_{};
    bool layout_loaded_{};
    std::optional<std::string> layout_error_; // reported on first use, never while the switch is off
    bool layout_error_reported_{};
    bool loaded_{};
    std::optional<presentation::ui::build_tray::TrayLayout> layout_;
    presentation::ui::build_tray::TrayGroups groups_;
    presentation::ui::build_tray::CategoryEnum categories_;
    presentation::ui::build_tray::TypeIndex types_;
    std::size_t type_count_{};

    godot::Control* panel_{};
    godot::TextureRect* panel_back_{};
    godot::TextureRect* panel_frame_{};
    EawrHudButton* handle_{};
    EawrHudText* handle_text_{};
    EawrHudButton* close_{};
    EawrHudText* close_text_{};
    std::string close_art_;              // the Art_Component the close button uses; empty: none of them in this mod
    godot::Rect2 close_rect_;
    std::uint64_t close_presses_{};
    // GBT-14: each queue button's place as the bar put it and as last lifted, so a lift never accumulates and a bar
    // that re-places a button (its cached key changed) is followed.
    // Built once from State::queue_buttons (they are made once with the bar); no per-frame allocation or lookup.
    struct Lifted final {
        godot::Control* button{};
        std::size_t domain{};  // index into State::queue_buttons: 0 land, 1 space
        godot::Vector2 base;
        std::optional<godot::Vector2> applied;
    };
    std::vector<Lifted> lifted_;
    std::uint64_t lifted_for_{}; // the instance id of the command bar whose buttons lifted_ holds (no address reuse)
    std::array<float, 2> lifts_{}; // [space, land]
    // The panel's back and frame canvas items (Back_Source Bar), one per sampled shell mesh, in the bar's order.
    std::vector<godot::RID> backdrop_items_;
    godot::Ref<godot::CanvasItemMaterial> additive_;
    std::string backdrop_source_;        // bar, component or none, for the report
    std::size_t back_layers_{}, frame_layers_{};
    std::vector<Entry> entries_;
    std::vector<EawrHudText*> headers_;
    std::vector<godot::TextureRect*> dividers_;
    bool open_{};
    std::size_t omitted_{};
    std::size_t omitted_by_layout_{};
    bool compact_{};
    bool handle_docked_{};
    bool dock_warned_{};
    std::uint64_t handle_presses_{};
    std::uint64_t key_presses_{};
    std::string bound_key_;          // the binding text the keycode below was resolved from
    std::int64_t bound_keycode_{};
    std::size_t groups_shown_{};
    std::size_t rows_shown_{};
    std::size_t entries_shown_{};
    godot::Rect2 panel_rect_, handle_rect_;
    std::optional<LayoutKey> shown_key_;
    std::optional<HandleKey> handle_key_;
    std::vector<viewer::galactic::BuildChoiceComponent> handle_choices_;
    std::optional<godot::Rect2> bar_;
    std::optional<std::pair<std::uint64_t, std::size_t>> lost_reported_;
    std::vector<viewer::galactic::BuildChoiceComponent> shown_choices_;
};

} // namespace eawr::presentation::godot_backend
