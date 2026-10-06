#pragma once

// #455 (P2 gap 4, #83): the command bar's minimap in Godot. The engine-free presentation::ui minimap
// model says what to draw (docs/behaviour/foc-minimap.md); this draws it inside the shell's `radar`
// mesh and turns the pointer on it into camera moves and move orders:
//
// - MM-13: the fog layer, the backdrop grid, the unit blips (their icons tinted with their colour,
//   turned by their facing) and the camera's outline as white lines, in that order;
// - MM-11: a left press looks at the point under the pointer; a left drag that has moved 12 pixels
//   follows the pointer while it stays on the minimap; a right release hands the point to the move
//   handler. Only the radar mesh takes the pointer.

#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/minimap.hpp"
#include "eawr/presentation/ui/selection.hpp"
#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

class EawrMinimap final : public godot::Control {
    GDCLASS(EawrMinimap, godot::Control)

public:
    struct Setup final {
        data::ui::ReferenceRect rect; // the radar mesh, shell units
        std::string backdrop;
        data::ui::Rgba8 background{12, 30, 51, 255}; // MM-14
        std::string warning_icon;
        float warning_scale{1.0F};
        // A command bar texture (MT_CommandBar, then files) at texel size; null when missing.
        std::function<godot::Ref<godot::Texture2D>(const std::string& texture)> texture;
        std::function<presentation::ui::ShellPlacement()> placement;
    };
    struct Frame final {
        struct Warning final { presentation::ui::MinimapPoint centre; double age{}; };
        std::vector<presentation::ui::MinimapBlip> blips;
        std::vector<Warning> warnings;
        std::optional<std::array<presentation::ui::MinimapPoint, 4>> guide;
    };

    EawrMinimap();
    ~EawrMinimap() override;
    void setup(Setup setup);
    void show(Frame frame);
    void prepare_orders(const vfs::Vfs& filesystem);
    void move_feedback(presentation::ui::MinimapPoint centre, presentation::ui::OrderMode mode, bool double_click);
    void order_frame(double seconds);
    // The fog layer's shown texels (MinimapFog::texels); re-uploaded when a pass completed.
    void set_fog(std::span<const std::uint8_t> texels, std::uint32_t width, std::uint32_t height, std::uint64_t pass);
    void set_hazards(std::span<const presentation::ui::MinimapHazard> hazards,
        const presentation::ui::MinimapExtents& extents, const presentation::ui::MinimapSettings& settings);
    void set_look(std::function<void(presentation::ui::MinimapPoint)> look) { look_ = std::move(look); }
    void set_move(std::function<void(presentation::ui::MinimapPoint, bool)> move) { move_ = std::move(move); }

    // The radar mesh on the screen, in viewport pixels.
    [[nodiscard]] godot::Rect2 minimap_rect() const;
    // The minimap's size in whole pixels (the fog layer's texel size, MM-10).
    [[nodiscard]] std::array<std::uint32_t, 2> pixel_size() const;
    [[nodiscard]] godot::Vector2 to_screen(presentation::ui::MinimapPoint point) const;
    [[nodiscard]] presentation::ui::MinimapPoint to_minimap(const godot::Vector2& point) const;
    // The "minimap" object of the HUD report.
    [[nodiscard]] std::string report_json() const;

    bool _has_point(const godot::Vector2& point) const override;
    void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
    void _notification(int what);
    void _draw() override;

protected:
    static void _bind_methods() {}

private:
    void look_at(const godot::Vector2& at, const char* why);
    struct OrderArt final {
        struct Sample final {
            godot::PackedVector2Array points, uvs;
            godot::PackedColorArray colours;
            godot::PackedInt32Array indices;
        };
        std::string name, model;
        double duration{}, scale{};
        bool singleton{};
        godot::Ref<godot::Texture2D> texture;
        std::vector<Sample> samples; // OF-04: baked at load, never allocated while drawing
    };
    struct OrderMarker final {
        godot::RID item;
        presentation::ui::MinimapPoint centre;
        double born{};
        std::size_t art{}, sample{};
        bool active{};
    };
    std::array<OrderArt, 3> order_art_;
    std::array<OrderMarker, 32> order_markers_;
    godot::Ref<godot::CanvasItemMaterial> order_material_;
    godot::PackedInt32Array order_empty_bones_;
    godot::PackedFloat32Array order_empty_weights_;
    godot::RID order_layer_;
    double order_seconds_{};
    bool order_events_enabled_{};
    std::uint64_t orders_shown_{}, orders_expired_{}, orders_replaced_{};

    Setup setup_;
    Frame frame_;
    godot::Ref<godot::Texture2D> backdrop_;
    // The backdrop as its own mipmapped texture, so it can repeat (MM-13).
    godot::Ref<godot::ImageTexture> backdrop_tiles_;
    // Fog and backdrop, drawn behind the control's own blips and outline with repeat and mipmaps.
    godot::RID layers_;
    godot::Ref<godot::ImageTexture> fog_;
    godot::Ref<godot::ImageTexture> hazards_;
    std::vector<presentation::ui::MinimapHazard> hazard_inputs_;
    std::array<double, 4> hazard_extents_{};
    std::array<std::uint32_t, 2> hazard_size_{};
    std::size_t hazard_pixels_{};
    std::uint64_t fog_pass_{};
    std::uint32_t fog_width_{};
    std::uint32_t fog_height_{};
    std::function<void(presentation::ui::MinimapPoint)> look_;
    std::function<void(presentation::ui::MinimapPoint, bool)> move_;
    std::optional<godot::Vector2> left_press_;
    bool dragged_{};
    bool right_down_{};
    bool right_double_click_{};
    godot::Vector2 laid_out_{-1.0F, -1.0F};
    std::uint64_t frames_{};
    std::uint64_t looks_{};
    std::uint64_t drags_{};
    std::uint64_t moves_{};
    std::size_t icons_missing_{};
    std::vector<std::string> log_;
};

} // namespace eawr::presentation::godot_backend
