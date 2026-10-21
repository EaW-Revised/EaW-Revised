#pragma once

// Report the actual, closed controls after Godot has applied minimum sizes. Inspect frame
// children rather than the ID map: resource labels can share an ID, and added controls have none.
#include "ui/dialog_builder.hpp"
#include <godot_cpp/classes/font.hpp>
#include <sstream>

namespace eawr::presentation::godot_backend {
inline std::string dialog_rectangles(const BuiltDialog& dialog) {
    const auto rectangle = [](const godot::Rect2& rect) {
        std::ostringstream out;
        out << '[' << rect.position.x << ',' << rect.position.y << ',' << rect.size.x << ',' << rect.size.y << ']';
        return out.str();
    };
    std::ostringstream out;
    out << "{\"dialog\":" << rectangle(dialog.frame->get_global_rect()) << ",\"controls\":[";
    bool first = true;
    for (int index = 0; index < dialog.frame->get_child_count(); ++index) {
        auto* control = godot::Object::cast_to<godot::Control>(dialog.frame->get_child(index));
        if (!control || !control->is_visible_in_tree()) continue;
        std::string kind = "added";
        const BuiltGadget* authored = nullptr;
        for (const auto& gadget : dialog.gadgets) if (gadget.control == control) {
            kind = gadget.kind;
            authored = &gadget;
        }
        const auto name = godot::String(control->get_name()).utf8();
        out << (first ? "" : ",") << "{\"id\":\"" << name.get_data() << "\",\"kind\":\"" << kind
            << "\",\"rect\":" << rectangle(control->get_global_rect());
        if (authored) {
            const auto& rect = authored->rect;
            const auto transform = dialog.frame->get_global_transform();
            const auto position = transform.xform(godot::Vector2(static_cast<float>(rect.x - dialog.layout.frame.x),
                                                                 static_cast<float>(rect.y - dialog.layout.frame.y)));
            const auto extent = transform.basis_xform(godot::Vector2(static_cast<float>(rect.width), static_cast<float>(rect.height)));
            out << ",\"authored\":" << rectangle(godot::Rect2(position, extent));
        }
        out << '}';
        first = false;
    }
    out << "],\"combos\":[";
    first = true;
    for (const auto& gadget : dialog.gadgets) {
        if (gadget.kind != "combo" || !gadget.control) continue;
        const auto* control = gadget.control;
        const auto font = control->get_theme_font("Combo_Box");
        out << (first ? "" : ",") << "{\"id\":\"" << gadget.id << "\",\"kind\":\"" << gadget.kind
            << "\",\"rect\":" << rectangle(control->get_global_rect())
            << ",\"font\":" << (font.is_valid() ? font->get_instance_id() : 0)
            << ",\"font_size\":" << control->get_theme_font_size("Combo_Box")
            << ",\"font_color\":\"" << control->get_theme_color("Combo_Box_top").to_html().utf8().get_data()
            << "\",\"outline_size\":" << control->get_theme_constant("Combo_Box_outline") << '}';
        first = false;
    }
    out << "]}";
    return out.str();
}
} // namespace eawr::presentation::godot_backend
