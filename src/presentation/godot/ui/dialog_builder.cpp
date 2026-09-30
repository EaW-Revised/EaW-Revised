#include "ui/dialog_builder.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;
namespace data = eawr::data::ui;

[[nodiscard]] bool has_style(const data::DialogControl& control, const std::string_view term) {
    return std::any_of(control.style.begin(), control.style.end(),
                       [term](const std::string& written) { return written == term; });
}

[[nodiscard]] bool iequal(const std::string_view left, const std::string_view right) {
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(), [](const char a, const char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

[[nodiscard]] std::string kind_of(const data::DialogControl& control) {
    const std::string_view statement = control.statement;
    if (statement == "PUSHBUTTON" || statement == "DEFPUSHBUTTON" || statement == "PUSHBOX") return "button";
    if (statement == "LTEXT" || statement == "RTEXT" || statement == "CTEXT") return "label";
    if (statement == "GROUPBOX") return "group";
    if (statement == "EDITTEXT") return "edit";
    if (statement == "COMBOBOX") return "combo";
    if (statement == "LISTBOX") return "list";
    if (statement != "CONTROL") return {};
    const std::string_view name = control.class_name;
    if (iequal(name, "Button")) {
        for (const std::string_view check : {"BS_AUTOCHECKBOX", "BS_CHECKBOX", "BS_AUTO3STATE", "BS_3STATE"}) {
            if (has_style(control, check)) return "check";
        }
        for (const std::string_view radio : {"BS_AUTORADIOBUTTON", "BS_RADIOBUTTON"}) {
            if (has_style(control, radio)) return "radio";
        }
        return has_style(control, "BS_GROUPBOX") ? "group" : "button";
    }
    if (iequal(name, "msctls_trackbar32")) return "slider";
    if (iequal(name, "msctls_progress32")) return "bar";
    if (iequal(name, "IMEEditBox")) return "edit";
    if (iequal(name, "Static")) return "label";
    return {};
}

[[nodiscard]] String godot_text(const std::string& value) {
    return String::utf8(value.c_str(), static_cast<int64_t>(value.size()));
}

// The frame's left and top thickness on screen, for the UI-L4 margins.
[[nodiscard]] model::FrameBorder frame_border(const model::ThemeModel& theme, const std::string& style) {
    model::FrameBorder border;
    if (const model::ThemeTexture* left = theme.texture(style, "Frame_Left");
        left != nullptr && left->origin == model::TextureOrigin::atlas) {
        border.left = left->rectangle.width * theme.scale_x;
    }
    if (const model::ThemeTexture* top = theme.texture(style, "Frame_Top");
        top != nullptr && top->origin == model::TextureOrigin::atlas) {
        border.top = top->rectangle.height * theme.scale_y;
    }
    return border;
}

} // namespace

BuiltDialog build_dialog(const data::Dialog& dialog, const model::ThemeModel& theme, data::TextLookup* text,
                         const model::Placement placement) {
    BuiltDialog built;
    const std::string frame_style = theme.variation(dialog, nullptr);
    const model::RcRect size{0, 0, dialog.rect.width, dialog.rect.height};
    built.layout = model::place_dialog(size, placement, theme.space, frame_border(theme, frame_style));
    const model::PixelRect& frame_rect = built.layout.frame;
    built.frame = memnew(EawrUiFrame);
    built.frame->set_name(godot_text(dialog.name));
    built.frame->set_theme_type_variation(godot_text(frame_style));
    built.frame->set_position(Vector2(static_cast<float>(frame_rect.x), static_cast<float>(frame_rect.y)));
    built.frame->set_size(Vector2(static_cast<float>(frame_rect.width), static_cast<float>(frame_rect.height)));

    for (const data::DialogControl& control : dialog.controls) {
        BuiltGadget gadget;
        gadget.id = control.id_name;
        gadget.statement = control.statement;
        gadget.class_name = control.class_name;
        gadget.kind = kind_of(control);
        gadget.variation = theme.variation(dialog, &control);
        const model::RcRect rc{control.rect.x, control.rect.y, control.rect.width, control.rect.height};
        gadget.rect = model::place_gadget(built.layout, rc);
        if (gadget.kind == "combo") {
            // A combo's `.rc` height includes its drop-down list; closed, it is
            // as high as its text box texture.
            const model::ThemeTexture* box = theme.texture(gadget.variation, "Combo_Box_Text_Box");
            if (box != nullptr && box->origin == model::TextureOrigin::atlas) {
                gadget.rect.height = box->rectangle.height * theme.scale_y;
            }
        }
        if (control.text && !control.text->empty()) {
            gadget.caption = text != nullptr ? data::to_utf8(text->text(*control.text)) : *control.text;
        }
        const String caption = godot_text(gadget.caption);
        Control* made = nullptr;
        if (gadget.kind == "button") {
            auto* button = memnew(EawrUiButton);
            button->set_text(caption);
            made = button;
        } else if (gadget.kind == "check" || gadget.kind == "radio") {
            auto* check = memnew(EawrUiCheck);
            check->set_text(caption);
            check->set_radio(gadget.kind == "radio");
            made = check;
        } else if (gadget.kind == "label") {
            auto* label = memnew(EawrUiLabel);
            label->set_text(caption);
            label->set_role(godot_text(std::string(data::to_string(data::font_role(control)))));
            label->set_alignment(control.statement == "RTEXT"   ? HORIZONTAL_ALIGNMENT_RIGHT
                                 : control.statement == "CTEXT" ? HORIZONTAL_ALIGNMENT_CENTER
                                                                : HORIZONTAL_ALIGNMENT_LEFT);
            made = label;
        } else if (gadget.kind == "group") {
            auto* group = memnew(EawrUiFrame);
            group->set_small(true);
            made = group;
        } else if (gadget.kind == "edit") {
            auto* edit = memnew(EawrUiEdit);
            edit->set_ime(iequal(control.class_name, "IMEEditBox"));
            made = edit;
        } else if (gadget.kind == "combo") {
            made = memnew(EawrUiCombo);
        } else if (gadget.kind == "list") {
            made = memnew(EawrUiList);
        } else if (gadget.kind == "slider") {
            auto* slider = memnew(EawrUiSlider);
            slider->set_max(100.0);
            made = slider;
        } else if (gadget.kind == "bar") {
            made = memnew(EawrUiBar);
        }
        gadget.control = made;
        if (made != nullptr) {
            made->set_name(godot_text(control.id_name));
            made->set_theme_type_variation(godot_text(gadget.variation));
            made->set_position(Vector2(static_cast<float>(gadget.rect.x - frame_rect.x),
                                       static_cast<float>(gadget.rect.y - frame_rect.y)));
            made->set_size(Vector2(static_cast<float>(gadget.rect.width), static_cast<float>(gadget.rect.height)));
            if (has_style(control, "NOT WS_VISIBLE")) made->set_visible(false);
            if (has_style(control, "WS_DISABLED")) {
                if (auto* button = Object::cast_to<BaseButton>(made)) button->set_disabled(true);
                if (auto* edit = Object::cast_to<LineEdit>(made)) edit->set_editable(false);
            }
            built.frame->add_child(made);
        }
        built.gadgets.push_back(std::move(gadget));
    }
    return built;
}

} // namespace eawr::presentation::godot_backend
