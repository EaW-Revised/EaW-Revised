#pragma once

#include "ui/kit.hpp"

#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace eawr::presentation::godot_backend {

// Drawing only: adapters supply admitted portrait, count text and progress facts.
// CARD-1/8 and WBP-36 keep their caller-authored boxes, tint and completion policy.
class PortraitCellRenderer final {
public:
    struct Progress final {
        godot::Ref<godot::Texture2D> texture;
        godot::Rect2 box;
        double completion{1.0};
    };
    struct Text final {
        KitTextStyle style;
        godot::String value;
        godot::Rect2 box;
        godot::HorizontalAlignment alignment{godot::HORIZONTAL_ALIGNMENT_CENTER};
        float scale{1.0F};
    };
    static void draw_portrait(godot::CanvasItem& owner, const godot::Ref<godot::Texture2D>& texture,
                             const godot::Rect2& box, const godot::Color& tint,
                             const std::optional<Progress>& progress = std::nullopt) {
        if (texture.is_null()) return;
        owner.draw_texture_rect(texture, box, false, tint);
        if (progress && progress->texture.is_valid() && progress->completion < 1.0)
            draw_progress(owner, *progress);
    }
    void draw_text(godot::CanvasItem& owner, const Text& text) {
        text_.draw(owner, text.style, text.value, text.box, text.alignment, true, false, text.scale, 1.0F);
    }
private:
    static void draw_progress(godot::CanvasItem& owner, const Progress& progress) {
        using godot::Color;
        using godot::PackedColorArray;
        using godot::PackedVector2Array;
        using godot::Vector2;
        static constexpr std::array<std::array<float, 2>, 9> rim{{
            {0.5F, 0}, {1, 0}, {1, 0.5F}, {1, 1}, {0.5F, 1}, {0, 1}, {0, 0.5F}, {0, 0}, {0.5F, 0}}};
        const double eighths = std::clamp(progress.completion, 0.0, 1.0) * 8.0;
        const auto first = static_cast<std::size_t>(std::floor(eighths));
        const auto part = static_cast<float>(eighths - static_cast<double>(first));
        PackedVector2Array points;
        PackedVector2Array uvs;
        const auto add = [&](float u, float v) {
            uvs.push_back(Vector2(u, v));
            points.push_back(progress.box.position + Vector2(u * progress.box.size.x, v * progress.box.size.y));
        };
        add(0.5F, 0.5F);
        add(rim[first][0] + (rim[first + 1][0] - rim[first][0]) * part,
            rim[first][1] + (rim[first + 1][1] - rim[first][1]) * part);
        for (auto index = first + 1; index < rim.size(); ++index) add(rim[index][0], rim[index][1]);
        PackedColorArray colours;
        colours.push_back(Color(1, 1, 1, 1));
        if (points.size() >= 3) owner.draw_polygon(points, colours, uvs, progress.texture);
    }
    KitText text_;
};

} // namespace eawr::presentation::godot_backend
