#pragma once
#include "tactical_hud.hpp"

#include "perf_overlay_view.hpp"

#include "ui/theme_builder.hpp"

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
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <map>
#include <sstream>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace model = presentation::ui;

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
    void _pressed() override {
        ++presses_;
        if (action_) action_();
        else UtilityFunctions::print("[eawr-hud] options pressed (P2-20a stub: the in-game menu is P2-20e)");
    }
    // #459: what a press does (the time panel's buttons); the options button has none yet.
    void set_action(std::function<void()> action) { action_ = std::move(action); }
    [[nodiscard]] int presses() const { return presses_; }
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
    static void _bind_methods() {}

private:
    int presses_{};
    TextureRect* flash_{};
    bool flashing_{};
    double flash_remaining_{};
    Rect2 hit_rect_;
    std::function<void()> action_;
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
    // #459: the time panel's live buttons, and #453/#459's overlay.
    EawrHudButton* pause_button{};
    EawrHudButton* fast_forward_button{};
    EawrBattleOverlay* overlay{};
    TacticalHud::TimeHandlers time_handlers;
    TacticalHud::TimeView time_view;
    bool time_view_set{};
    // #848 V-5b: an overview level is on.
    bool overview{};
    model::BattleMessageLooks message_looks;
    std::vector<std::string> time_textures; // "button state: texture (origin)"
    // #425: the unit cards, and what they look up: object types (Icon_Name, Text_ID), the text DB
    // and the command bar's textures, each decoded once.
    EawrUnitCards* cards{};
    const data::Catalog* objects{};
    std::optional<data::ui::TextDatabase> text_database;
    model::StandaloneTextures standalone;
    std::map<std::string, Ref<Texture2D>, std::less<>> card_textures;
    std::map<std::string, std::pair<std::string, String>, std::less<>> card_types; // Icon_Name, display name
    model::ResolvedFont card_face;
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
    std::function<void(double, double)> minimap_move;
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
