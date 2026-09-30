#pragma once

// The Godot UI kit (ticket UI-06 #229, docs/ui/ui-layer.md sections 3.2 and
// 3.6). Leaf Controls draw the retail skin from theme items that
// ThemeBuilder (theme_builder.hpp) fills from the GUIDialogs.xml data:
//
// - textures are theme icons named as the skin slots (`Button_Left_Pressed`),
//   already sized to the UI-L3 scale, so a piece draws at its texture size;
// - fonts are theme fonts named as the font roles (`Push_Button`), with
//   `<role>_top` and `<role>_bottom` colours (UI-F4), `<role>_emboss` and
//   `<role>_outline` flags, and `<role>_cell_ascent`/`_cell_descent`: the
//   GDI text cell that places the text as the original does (0: the
//   engine's metrics);
// - the constant `scale_permille` is the UI-L3 scale times 1000, for sizes
//   that are not textures (text emboss and outline, list padding).
//
// Every kit Control starts at the `EawrUi` theme type; a per-dialog or
// per-control skin set is a type variation of it (ThemeModel::variation).
// The frame, button, label, check/radio, slider and bar draw themselves; the
// list, combo and edit keep Godot's ItemList, OptionButton and LineEdit
// behaviour and apply the skin as theme overrides plus a drawn frame.

#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/range.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>

#include <string_view>

namespace eawr::presentation::godot_backend {

// Metadata key of the theme builder's empty marker texture (a slot written
// "none" or one that resolves nowhere); kit pieces treat it as absent.
inline constexpr const char* empty_piece_meta = "eawr_empty_piece";

// A state the gallery can pin; automatic follows the pointer and the control.
enum class KitState : int { automatic = -1, normal = 0, hover = 1, pressed = 2, disabled = 3 };

// The resolved look of one font role on one Control.
struct KitTextStyle final {
    godot::Ref<godot::Font> font;
    int size{};
    godot::Color top;
    godot::Color bottom;
    bool emboss{};
    bool outline{};
    int cell_ascent{};
    int cell_descent{};
};

[[nodiscard]] KitTextStyle kit_text_style(const godot::Control& control, std::string_view role);

// Text drawn into a child canvas item of its owner, so a vertical gradient
// (UI-F4) can use a material without touching the owner's textures.
class KitText final {
public:
    KitText() = default;
    KitText(const KitText&) = delete;
    KitText& operator=(const KitText&) = delete;
    ~KitText();

    void draw(godot::CanvasItem& owner, const KitTextStyle& style, const godot::String& text, const godot::Rect2& box,
              godot::HorizontalAlignment horizontal, bool centre_vertically, bool wrap, float scale, float alpha);

private:
    godot::RID item_;
    godot::Ref<godot::ShaderMaterial> material_;
};

// The 16-piece dialog frame around the rect (the pieces sit outside it, the
// background stretches inside), or with `small` the 8-piece frame inside it.
class EawrUiFrame final : public godot::Control {
    GDCLASS(EawrUiFrame, godot::Control)

public:
    EawrUiFrame();
    void set_small(bool small);
    [[nodiscard]] bool is_small() const { return small_; }
    void _draw() override;

protected:
    static void _bind_methods() {}

private:
    bool small_{};
};

// Draws a small frame (Small_Frame_*) inside `rect` on `control`'s canvas item.
void draw_small_frame(godot::Control& control, const godot::Rect2& rect, bool background);

// Push button: Button_Left/Middle/Right in the normal, mouse-over, pressed and
// disabled sets, caption in the Push_Button role. The pieces keep their
// texture height from the top of the rect, as retail draws them.
class EawrUiButton final : public godot::BaseButton {
    GDCLASS(EawrUiButton, godot::BaseButton)

public:
    EawrUiButton();
    void set_text(const godot::String& text);
    [[nodiscard]] godot::String get_text() const { return text_; }
    void set_forced_state(KitState state);
    [[nodiscard]] KitState state() const;
    void _draw() override;
    [[nodiscard]] godot::Vector2 _get_minimum_size() const override;

protected:
    static void _bind_methods() {}

private:
    godot::String text_;
    KitState forced_{KitState::automatic};
    KitText caption_;
};

// Static text in one font role (Global_Default, L_Text, R_Text, ...) with the
// role's gradient, emboss and outline.
class EawrUiLabel final : public godot::Control {
    GDCLASS(EawrUiLabel, godot::Control)

public:
    EawrUiLabel();
    void set_text(const godot::String& text);
    void set_role(const godot::String& role);
    void set_alignment(godot::HorizontalAlignment alignment);
    void set_wrap(bool wrap);
    void _draw() override;

protected:
    static void _bind_methods() {}

private:
    godot::String text_;
    godot::String role_{"Global_Default"};
    godot::HorizontalAlignment alignment_{godot::HORIZONTAL_ALIGNMENT_LEFT};
    bool wrap_{true};
    KitText caption_;
};

// Check box (Check_On/Off) or, with `radio`, radio button (Radio_On/Off and
// Radio_Mouse_Over); the caption takes the Global_Default role.
class EawrUiCheck final : public godot::BaseButton {
    GDCLASS(EawrUiCheck, godot::BaseButton)

public:
    EawrUiCheck();
    void set_text(const godot::String& text);
    void set_radio(bool radio);
    void set_forced_state(KitState state);
    [[nodiscard]] KitState state() const;
    void _draw() override;
    [[nodiscard]] godot::Vector2 _get_minimum_size() const override;

protected:
    static void _bind_methods() {}

private:
    godot::String text_;
    bool radio_{};
    KitState forced_{KitState::automatic};
    KitText caption_;
};

// Slider (msctls_trackbar32): Dial_Minus, the Dial_Left/Middle/Right track
// with the Dial_Tab thumb, and Dial_Plus. The end buttons step the value.
class EawrUiSlider final : public godot::Range {
    GDCLASS(EawrUiSlider, godot::Range)

public:
    EawrUiSlider();
    void set_forced_states(KitState minus, KitState plus);
    void _draw() override;
    void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
    void _notification(int what);

protected:
    static void _bind_methods() {}

private:
    enum class Part : int { none, minus, track, plus };
    struct Layout final {
        godot::Rect2 minus, track, plus, tab;
    };
    [[nodiscard]] Layout layout() const;
    [[nodiscard]] Part part_at(godot::Vector2 position) const;
    void set_from_track(float x);

    KitState forced_minus_{KitState::automatic};
    KitState forced_plus_{KitState::automatic};
    Part hovered_{Part::none};
    Part held_{Part::none};
};

// Progress bar (msctls_progress32): Progress_Bar_Left, Middle_On up to the
// value and Middle_Off after it (both tiled), Progress_Bar_Right.
class EawrUiBar final : public godot::Range {
    GDCLASS(EawrUiBar, godot::Range)

public:
    EawrUiBar();
    void _draw() override;

protected:
    static void _bind_methods() {}
};

// List box: ItemList with the small frame, the List_Box font and the Scroll_*
// scroll bar, whose Scroll_Tab keeps its size as the original's does. The
// selection bar matches the FoC skirmish-setup capture.
class EawrUiList final : public godot::ItemList {
    GDCLASS(EawrUiList, godot::ItemList)

public:
    EawrUiList();
    void _draw() override;
    void _notification(int what);

protected:
    static void _bind_methods() {}

private:
    void apply_skin();
    void draw_scroll_tab();
    bool applying_{};
};

// Combo box: OptionButton on Combo_Box_Text_Box with Combo_Box_Left_Cap and the
// Combo_Box_Popdown_Button states, in the Combo_Box font.
class EawrUiCombo final : public godot::OptionButton {
    GDCLASS(EawrUiCombo, godot::OptionButton)

public:
    EawrUiCombo();
    void set_forced_state(KitState state);
    void _draw() override;
    void _notification(int what);

protected:
    static void _bind_methods() {}

private:
    void apply_skin();
    KitState forced_{KitState::automatic};
    bool applying_{};
};

// Edit box: LineEdit inside the small frame, in the Edit_Box font (or
// IME_Edit_Box with `ime`).
class EawrUiEdit final : public godot::LineEdit {
    GDCLASS(EawrUiEdit, godot::LineEdit)

public:
    EawrUiEdit();
    void set_ime(bool ime);
    void _draw() override;
    void _notification(int what);

protected:
    static void _bind_methods() {}

private:
    void apply_skin();
    bool ime_{};
    bool applying_{};
};

// Registers the kit classes with ClassDB (scene initialisation level).
void register_ui_kit_classes();

} // namespace eawr::presentation::godot_backend
