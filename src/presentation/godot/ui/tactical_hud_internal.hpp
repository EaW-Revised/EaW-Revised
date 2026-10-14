#pragma once
#include "tactical_hud.hpp"

#include "perf_overlay_view.hpp"

#include "ui/theme_builder.hpp"
#include "ui/portrait_cell.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/data/ui/shell_anchors.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/ui/battle_messages.hpp"
#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/theme.hpp"

#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <godot_cpp/classes/color_rect.hpp>
#include <cmath>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace model = presentation::ui;

class EawrSpecialWeaponDial final : public Control {
    GDCLASS(EawrSpecialWeaponDial, Control)
public:
    Ref<Texture2D> texture;
    double completion{1.0};
    void _draw() override {
        if (texture.is_null() || completion >= 1.0) return;
        // WSW-29 / ABE-7: reuse the command bar's clockwise recharge fan.
        static const std::array<Vector2, 9> rim{{{0.5F, 0}, {1, 0}, {1, 0.5F}, {1, 1}, {0.5F, 1},
            {0, 1}, {0, 0.5F}, {0, 0}, {0.5F, 0}}};
        const double eighths = std::clamp(completion, 0.0, 1.0) * 8.0;
        const auto first = static_cast<std::size_t>(std::floor(eighths));
        const float part = static_cast<float>(eighths - static_cast<double>(first));
        PackedVector2Array points, uvs;
        const auto add = [&](Vector2 uv) { uvs.push_back(uv); points.push_back(uv * get_size()); };
        add({0.5F, 0.5F});
        add(rim[first].lerp(rim[first + 1], part));
        for (auto i = first + 1; i < rim.size(); ++i) add(rim[i]);
        PackedColorArray colours;
        colours.push_back(Color(1, 1, 1, 1));
        draw_polygon(points, colours, uvs, texture);
    }
protected:
    static void _bind_methods() {}
};

// Prepared art from the caller's UI resource context. Lookup and lifetime remain
// with that context; authored state/portrait/overlay wiring is common to both modes.
struct HudButtonArt final {
    Ref<Texture2D> normal, hover, pressed, disabled;
    struct Portrait final {
        Ref<Texture2D> texture;
        bool click_shift{};
        bool shared_cell{};
    };
    struct Overlays final {
        Ref<Texture2D> hover, selected, disabled;
        bool selected_additive{};
    };
    std::optional<Portrait> portrait;
    std::optional<Overlays> overlays;
};

// The options button: the component's four state textures. P2-20a only counts
// presses; the in-game menu it opens is P2-20e. The control spans the drawn art
// quad, but only its component's mesh rect takes the pointer (hover and click),
// as FoC picks the shell mesh, not the art (docs/ui/ui-layer.md 1.3).
class EawrHudButton final : public TextureButton {
    GDCLASS(EawrHudButton, TextureButton)

public:
    EawrHudButton() {
        set_ignore_texture_size(true);
        set_stretch_mode(STRETCH_SCALE);
        set_focus_mode(FOCUS_NONE);
        set_mouse_filter(MOUSE_FILTER_STOP);
    }
    void setup_art(const HudButtonArt& art) {
        set_texture_normal(art.normal);
        set_texture_hover(art.hover);
        set_texture_pressed(art.pressed);
        set_texture_disabled(art.disabled);
        if (art.portrait) {
            set_order_icon(art.portrait->texture, art.portrait->click_shift);
            set_portrait_cell_enabled(art.portrait->shared_cell);
        }
        if (art.overlays) set_special_overlays(art.overlays->hover, art.overlays->selected,
            art.overlays->disabled, art.overlays->selected_additive);
    }
    void _pressed() override {
        if (is_disabled() || !is_visible_in_tree() || (get_viewport() && get_viewport()->gui_is_dragging())) return;
        ++presses_;
        // WGP-182: right-only controls have a right handler; other controls use left release.
        const auto& cue = get_button_mask() == MOUSE_BUTTON_MASK_RIGHT ? right_click_sfx_ : click_sfx_;
        if (sound_ && !cue.empty()) sound_(cue);
        if (action_) action_();
        else UtilityFunctions::print("[eawr-hud] options pressed (P2-20a stub: the in-game menu is P2-20e)");
    }
    // #459: what a press does (the time panel's buttons); the options button has none yet.
    void set_action(std::function<void()> action) { action_ = std::move(action); }
    // #3075: the authored Click_SFX / Mouse_Over_SFX names and the owner's SFX sink.
    void set_sounds(std::string click, std::string mouse_over, std::function<void(const std::string&)> sound, std::string right_click = {}) {
        if (!sound_ && sound) connect("mouse_entered", callable_mp(this, &EawrHudButton::hover_sound));
        click_sfx_ = std::move(click); mouse_over_sfx_ = std::move(mouse_over);
        right_click_sfx_ = std::move(right_click); sound_ = std::move(sound);
    }
    void set_sounds(const data::ui::CommandBarComponent& component, std::function<void(const std::string&)> sound) {
        set_sounds(std::string(component.text(data::ui::Field::click_sfx)),
            std::string(component.text(data::ui::Field::mouse_over_sfx)), std::move(sound),
            std::string(component.text(data::ui::Field::right_click_sfx)));
    }
    void set_order_icon(const Ref<Texture2D>& texture, bool shift) {
        order_icon_ = memnew(TextureRect);
        order_icon_->set_mouse_filter(MOUSE_FILTER_IGNORE);
        order_icon_->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        order_icon_->set_stretch_mode(TextureRect::STRETCH_SCALE);
        order_icon_->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
        order_icon_->set_texture(texture);
        add_child(order_icon_);
        order_shift_ = shift;
        connect("button_down", callable_mp(this, &EawrHudButton::order_down));
        connect("button_up", callable_mp(this, &EawrHudButton::order_up));
    }
    void set_order_selected(bool selected) {
        order_selected_ = selected;
        set_pressed_no_signal(selected);
        shift_order_icon(selected);
        refresh_special_overlay();
    }
    // `selected_additive`: a component without Selected_Alpha adds its selected art instead of
    // blending it (PU-70), so the icon under it stays visible.
    void set_special_overlays(const Ref<Texture2D>& hover, const Ref<Texture2D>& selected, const Ref<Texture2D>& disabled,
                              bool selected_additive = false) {
        special_hover_ = hover; special_selected_ = selected; special_disabled_ = disabled;
        if (selected_additive) {
            special_additive_.instantiate();
            special_additive_->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
        }
        special_overlay_ = memnew(TextureRect);
        special_overlay_->set_mouse_filter(MOUSE_FILTER_IGNORE);
        special_overlay_->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        special_overlay_->set_stretch_mode(TextureRect::STRETCH_SCALE);
        add_child(special_overlay_);
        special_overlay_->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
        connect("mouse_entered", callable_mp(this, &EawrHudButton::refresh_special_overlay));
        connect("mouse_exited", callable_mp(this, &EawrHudButton::refresh_special_overlay));
        connect("button_down", callable_mp(this, &EawrHudButton::refresh_special_overlay));
        connect("button_up", callable_mp(this, &EawrHudButton::refresh_special_overlay));
        refresh_special_overlay();
    }
    void set_special_disabled(bool disabled) { set_disabled(disabled); refresh_special_overlay(); }
    // GHUD-S73: an authored state quad can differ from the retained icon's size.
    void set_special_overlay_box(const Rect2& box) {
        if (!special_overlay_) return;
        special_overlay_->set_anchors_preset(PRESET_TOP_LEFT);
        special_overlay_->set_position(box.position); special_overlay_->set_size(box.size);
    }
    [[nodiscard]] Rect2 special_overlay_rect() const {
        return special_overlay_ ? special_overlay_->get_global_rect() : Rect2{};
    }
    void set_order_shift_pixels(float pixels) { order_shift_pixels_ = pixels; }
    void set_icon_texture(const Ref<Texture2D>& texture) {
        if (order_icon_) order_icon_->set_texture(texture);
        if (portrait_cell_) queue_redraw();
    }
    // Keep the button's existing hit mask and press/drag ownership; share its drawing cell.
    void set_portrait_cell_enabled(bool enabled) {
        portrait_cell_ = enabled;
        if (order_icon_) order_icon_->set_visible(!enabled);
        queue_redraw();
    }
    void set_cell_text(PortraitCellRenderer::Text text) { cell_text_ = std::move(text); queue_redraw(); }
    [[nodiscard]] String cell_text() const { return cell_text_ ? cell_text_->value : String{}; }
    // Caller-prepared icon geometry also stays on the native child for inspection.
    void set_cell_portrait_box(const Rect2& box) {
        if (!order_icon_) return;
        cell_portrait_box_ = box;
        order_icon_->set_anchors_preset(PRESET_TOP_LEFT);
        order_icon_->set_position(box.position); order_icon_->set_size(box.size);
        if (portrait_cell_) queue_redraw();
    }
    [[nodiscard]] Rect2 cell_portrait_box() const {
        return order_icon_ ? Rect2(order_icon_->get_position(), order_icon_->get_size()) : Rect2{};
    }
    [[nodiscard]] Rect2 cell_text_box() const { return cell_text_ ? cell_text_->box : Rect2{}; }
    // Natural-size state quads are independent of a cell's linked hit region.
    void set_cell_state_art(HudButtonArt art, Vector2 centre, float scale) {
        cell_art_ = CellArt{std::move(art), centre, scale};
        set_texture_normal({}); set_texture_hover({}); set_texture_pressed({}); set_texture_disabled({});
        queue_redraw();
    }
    [[nodiscard]] bool shared_cell() const { return portrait_cell_; }
    void copy_cell_drawing_to(EawrHudButton& target, std::optional<String> count = {}, bool base_only = false,
        bool draw_base = true) const {
        if (!order_icon_) return;
        target.set_order_icon(base_only ? Ref<Texture2D>{} : order_icon_->get_texture(), false);
        target.set_portrait_cell_enabled(portrait_cell_);
        target.set_cell_portrait_box(cell_portrait_box());
        if (cell_text_ && !base_only) {
            auto text = *cell_text_;
            if (count) text.value = *count;
            target.set_cell_text(std::move(text));
        }
        if (cell_art_ && draw_base) {
            HudButtonArt art;
            art.normal = count || base_only ? cell_art_->art.normal : cell_state_texture();
            target.set_cell_state_art(std::move(art), cell_art_->centre, cell_art_->scale);
        }
    }
    void set_cell_progress(std::optional<PortraitCellRenderer::Progress> progress) {
        cell_progress_ = std::move(progress); queue_redraw();
    }
    void _draw() override {
        if (!portrait_cell_ || !order_icon_) return;
        if (cell_art_) {
            const auto texture = cell_state_texture();
            if (texture.is_valid()) {
                const auto size = texture->get_size() * cell_art_->scale;
                draw_texture_rect(texture, Rect2(cell_art_->centre - size * 0.5F, size), false);
            }
        }
        PortraitCellRenderer::draw_portrait(*this, order_icon_->get_texture(),
            Rect2(order_icon_->get_position(), order_icon_->get_size()), Color(1, 1, 1, 1), cell_progress_);
        if (cell_text_) cell_renderer_.draw_text(*this, *cell_text_);
    }
    [[nodiscard]] int presses() const { return presses_; }
    // HCB-12g: the layers a scripted case can read back: the icon child's texture, and which state
    // art is over it ("none", "hover", "selected" or "disabled").
    [[nodiscard]] bool icon_shown() const { return order_icon_ != nullptr && order_icon_->get_texture().is_valid(); }
    [[nodiscard]] const char* overlay_name() const {
        if (special_overlay_ == nullptr) return "none";
        return is_disabled() ? "disabled" : order_selected_ || is_pressed() ? "selected" : is_hovered() ? "hover" : "none";
    }
    void set_flash_texture(const Ref<Texture2D>& texture) {
        flash_ = memnew(TextureRect);
        flash_->set_mouse_filter(MOUSE_FILTER_IGNORE);
        flash_->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        flash_->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
        flash_->set_texture(texture);
        Ref<CanvasItemMaterial> material;
        material.instantiate();
        material->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
        flash_->set_material(material);
        flash_->hide();
        add_child(flash_);
    }
    void set_flashing(const bool on) {
        if (flashing_ == on) return;
        flashing_ = on;
        flash_remaining_ = 0.5;
        set_process(on);
        if (flash_ != nullptr) flash_->set_visible(on);
    }
    void _notification(const int what) {
        if (what != NOTIFICATION_PROCESS || !flashing_ || flash_ == nullptr) return;
        // TM-08: additive flash art fades every half second even while the sim is stopped.
        flash_remaining_ -= get_process_delta_time();
        while (flash_remaining_ <= 0.0) flash_remaining_ += 0.5;
        const float level = static_cast<float>(flash_remaining_ / 0.5);
        flash_->set_self_modulate(Color(level, level, level, 1.0F));
    }
    // The interactive rect in the button's local coordinates.
    void set_hit_rect(const Rect2& rect) { hit_rect_ = rect; }
    [[nodiscard]] const Rect2& hit_rect() const { return hit_rect_; }
    bool _has_point(const Vector2& point) const override { return hit_rect_.has_point(point); }

protected:
    static void _bind_methods() {
        ClassDB::bind_method(D_METHOD("set_portrait_cell_enabled", "enabled"), &EawrHudButton::set_portrait_cell_enabled);
    }

private:
    struct CellArt final { HudButtonArt art; Vector2 centre; float scale{1.0F}; };
    [[nodiscard]] Ref<Texture2D> cell_state_texture() const {
        const auto& art = cell_art_->art;
        if (is_disabled()) return art.disabled.is_valid() ? art.disabled : art.normal;
        if ((order_selected_ || is_pressed()) && art.pressed.is_valid()) return art.pressed;
        if (is_hovered() && art.hover.is_valid()) return art.hover;
        return art.normal;
    }
    std::optional<CellArt> cell_art_;
    void refresh_special_overlay() {
        if (!special_overlay_) return;
        const bool selected = !is_disabled() && (order_selected_ || is_pressed());
        special_overlay_->set_texture(is_disabled() ? special_disabled_
            : selected ? special_selected_ : is_hovered() ? special_hover_ : Ref<Texture2D>());
        special_overlay_->set_material(selected ? special_additive_ : Ref<CanvasItemMaterial>());
    }
    TextureRect* special_overlay_{};
    Ref<CanvasItemMaterial> special_additive_;
    Ref<Texture2D> special_hover_, special_selected_, special_disabled_;
    void shift_order_icon(bool down) {
        if (!order_icon_) return;
        const float shift = down && order_shift_ ? order_shift_pixels_ : 0.0F;
        if (cell_portrait_box_) {
            order_icon_->set_position(cell_portrait_box_->position + Vector2(0, shift));
            order_icon_->set_size(cell_portrait_box_->size);
        } else {
            order_icon_->set_offset(SIDE_TOP, shift);
            order_icon_->set_offset(SIDE_BOTTOM, shift);
        }
        if (portrait_cell_) queue_redraw();
    }
    void order_down() { shift_order_icon(true); }
    void order_up() { shift_order_icon(order_selected_); }
    TextureRect* order_icon_{};
    std::optional<Rect2> cell_portrait_box_;
    bool portrait_cell_{};
    PortraitCellRenderer cell_renderer_;
    std::optional<PortraitCellRenderer::Text> cell_text_;
    std::optional<PortraitCellRenderer::Progress> cell_progress_;
    bool order_shift_{};
    bool order_selected_{};
    float order_shift_pixels_{1.0F};
    int presses_{};
    TextureRect* flash_{};
    bool flashing_{};
    double flash_remaining_{};
    Rect2 hit_rect_;
    std::function<void()> action_;
    // #3075: once per hover entry, only from the normal state (not disabled or selected).
    void hover_sound() {
        if (sound_ && !mouse_over_sfx_.empty() && !is_disabled() && is_visible_in_tree() && !order_selected_ && !is_pressed()) sound_(mouse_over_sfx_);
    }
    std::string click_sfx_, mouse_over_sfx_, right_click_sfx_;
    std::function<void(const std::string&)> sound_;
};

struct TacticalHud::State final {
    explicit State(Options value);

    Options options;
    std::string failure;
    std::string shell_model;
    std::string atlas_path;
    model::HudShell shell;
    model::PlanetName planet;
    std::shared_ptr<FontProvider> fonts;
    std::array<Ref<Font>, 2> world_group_fonts;
    model::ResolvedFont planet_face;
    std::optional<assets::MegaTextureAtlas> atlas;
    std::unique_ptr<UiTextures> textures;
    std::vector<std::string> mesh_textures;   // "name: origin" per drawn mesh
    std::vector<std::string> button_textures; // "state: origin" for the options button
    struct PanelArt final {
        std::string name;
        std::string texture;
        std::string origin;
        data::ui::ReferenceRect quad; // shell units
    };
    std::vector<PanelArt> panel;
    std::vector<core::Diagnostic> diagnostics;
    std::size_t faceplates{};
    EawrUiHitMask* mask{};
    EawrTacticalHud* hud{};
    EawrHudButton* options_button{};
    EawrHudButton* movie_button{};
    EawrHudButton* beacon_button{};
    std::function<void()> beacon_handler;
    Control* movie_bands{};
    std::optional<float> movie_height;
    std::function<void()> movie_handler;
    bool movie{};
    // #459: the time panel's live buttons, and #453/#459's overlay.
    EawrHudButton* pause_button{};
    EawrHudButton* fast_forward_button{};
    EawrBattleOverlay* overlay{};
    TacticalHud::TimeHandlers time_handlers;
    TacticalHud::ReplayWriter replay_writer;
    std::function<void(std::string_view)> order_handler;
    std::map<std::string, EawrHudButton*, std::less<>> order_buttons;
    EawrHudButton* special_button{};
    EawrSpecialWeaponDial* special_dial{};
    model::SpecialWeaponButton special_view;
    std::function<void()> special_handler;
    std::string special_icon;
    EawrHudButton* retreat_button{};
    EawrSpecialWeaponDial* retreat_dial{};
    model::RetreatButtonView retreat_view;
    std::function<void()> retreat_handler;
    TacticalHud::TimeView time_view;
    bool time_view_set{};
    // #848 V-5b: an overview level is on.
    bool overview{};
    model::BattleMessageLooks message_looks;
    std::vector<std::string> time_textures; // "button state: texture (origin)"
    // #425: the unit cards, and what they look up: object types (Icon_Name, Text_ID), the text DB
    // and the command bar's textures, each decoded once.
    EawrUnitCards* cards{};
    std::optional<std::pair<std::uint64_t, std::string>> world_hover;
    std::string ability_hover_type;
    std::uint32_t ability_hover_kind{};
    std::size_t ability_hover_component{};
    const data::Catalog* objects{};
    std::optional<data::ui::TextDatabase> text_database;
    model::StandaloneTextures standalone;
    std::map<std::string, Ref<Texture2D>, std::less<>> card_textures;
    std::map<std::string, std::pair<std::string, String>, std::less<>> card_types; // Icon_Name, display name
    model::ResolvedFont card_face;
    std::map<std::string, model::UnitCardLooks, std::less<>> encyclopedia_types;
    const model::UnitCardLooks& encyclopedia_type(const std::string& type);
    const std::pair<std::string, String>& card_type(const std::string& type);
    // A command bar texture (the atlas, then files) at texel size, decoded once; null when missing.
    Ref<Texture2D> command_texture(const std::string& name, const std::string& use, bool quiet = false);
    // #454: the ability buttons.
    EawrAbilityButtons* abilities{};
    // #530: the build queue, the credits and the reinforcement pane.
    EawrProductionPanel* production{};
    // #455: the minimap and its model state.
    EawrMinimap* minimap{};
    model::MinimapSettings minimap_settings;
    model::MinimapFog minimap_fog;
    model::MinimapExtents minimap_extents;
    std::map<std::string, model::MinimapTypeLooks, std::less<>> minimap_types;
    std::function<void(double, double)> minimap_look;
    std::function<void(double, double, bool)> minimap_move;
    int world_presses{};
    struct Probe final {
        std::string name;
        Vector2 point;
        bool hud_hit{};
        int world{};
        int button{};
    };
    std::vector<Probe> probes;
    bool probed{};

    void run_probe();

    void warn(std::string code, std::string message, std::string path = {});
};

namespace tactical_hud_detail {
[[nodiscard]] String text(const std::string_view value);
[[nodiscard]] Rect2 rect2(const model::PixelRect& rect);
[[nodiscard]] Color colour(const data::ui::Rgba8& value);
} // namespace tactical_hud_detail

} // namespace eawr::presentation::godot_backend
