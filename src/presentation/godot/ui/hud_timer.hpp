#pragma once

#include "eawr/presentation/ui/hud_shell.hpp"
#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace eawr::presentation::godot_backend {

// HCB-62: caller-owned placement and progress; no simulation or card-selection
// dependency. Both timer components use the same renderer and XML-authored art.
struct HudTimerRenderer final {
    godot::Ref<godot::Texture2D> blank, icon, dial;
    godot::Ref<godot::CanvasItemMaterial> additive;
    godot::RID dial_item;
    godot::Rect2 icon_rect;
    std::optional<double> progress;

    ~HudTimerRenderer() {
        if (dial_item.is_valid()) godot::RenderingServer::get_singleton()->free_rid(dial_item);
    }
    void load(const presentation::ui::HudTimerStyle& style,
              const std::function<godot::Ref<godot::Texture2D>(const std::string&)>& texture) {
        if (!texture) return;
        if (!style.button.blank.empty()) blank = texture(style.button.blank);
        if (!style.button.normal.empty()) icon = texture(style.button.normal);
        if (!style.build.empty()) dial = texture(style.build);
    }
    void draw(godot::Control& parent, const presentation::ui::HudTimerStyle& style,
              godot::Vector2 centre, const double ui_scale, const std::optional<double> value) {
        using namespace godot;
        auto* server = RenderingServer::get_singleton();
        if (dial_item.is_valid()) server->canvas_item_clear(dial_item);
        progress = value;
        icon_rect = {};
        if (!value) return;
        const auto rect = [&](const Ref<Texture2D>& texture) {
            const Vector2 size = texture->get_size() * static_cast<float>(ui_scale * style.button.scale);
            return Rect2(centre - size * 0.5F, size);
        };
        const auto& c = style.colour;
        const Color tint(c.r / 255.0F, c.g / 255.0F, c.b / 255.0F, c.a / 255.0F);
        if (blank.is_valid()) parent.draw_texture_rect(blank, rect(blank), false, tint);
        if (icon.is_valid()) {
            icon_rect = rect(icon);
            parent.draw_texture_rect(icon, icon_rect, false, tint);
        }
        if (dial.is_null() || *value >= 1.0) return;
        if (!dial_item.is_valid()) {
            additive.instantiate();
            additive->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
            dial_item = server->canvas_item_create();
            server->canvas_item_set_parent(dial_item, parent.get_canvas_item());
            server->canvas_item_set_material(dial_item, additive->get_rid());
        }
        // ABE-7: the existing clockwise recharge fan, from top centre.
        static const std::array<Vector2, 9> rim{{{0.5F, 0}, {1, 0}, {1, 0.5F}, {1, 1}, {0.5F, 1},
            {0, 1}, {0, 0.5F}, {0, 0}, {0.5F, 0}}};
        const double eighths = std::clamp(*value, 0.0, 1.0) * 8.0;
        const auto first = static_cast<std::size_t>(std::floor(eighths));
        const float part = static_cast<float>(eighths - static_cast<double>(first));
        const auto box = rect(dial);
        PackedVector2Array points, uvs;
        const auto add = [&](const Vector2 uv) {
            uvs.push_back(uv);
            points.push_back(box.position + uv * box.size);
        };
        add({0.5F, 0.5F});
        add(rim[first].lerp(rim[first + 1], part));
        for (auto i = first + 1; i < rim.size(); ++i) add(rim[i]);
        PackedColorArray colours;
        colours.push_back(tint);
        server->canvas_item_add_polygon(dial_item, points, colours, uvs, dial->get_rid());
    }
};

} // namespace eawr::presentation::godot_backend
