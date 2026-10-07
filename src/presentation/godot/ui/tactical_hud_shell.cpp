#include "tactical_hud_internal.hpp"

namespace eawr::presentation::godot_backend {
using tactical_hud_detail::text;
using tactical_hud_detail::rect2;
using tactical_hud_detail::colour;

EawrTacticalHud::EawrTacticalHud() {
    set_mouse_filter(MOUSE_FILTER_IGNORE);
    set_focus_mode(FOCUS_NONE);
    set_anchors_and_offsets_preset(PRESET_FULL_RECT);
}

EawrTacticalHud::~EawrTacticalHud() {
    if (RenderingServer* rendering = RenderingServer::get_singleton()) {
        for (const RID& item : items_) rendering->free_rid(item);
        if (panel_item_.is_valid()) rendering->free_rid(panel_item_);
    }
}

void EawrTacticalHud::setup(Setup setup, TextureButton* options) {
    setup_ = std::move(setup);
    RenderingServer* rendering = RenderingServer::get_singleton();
    for (const Mesh& mesh : setup_.meshes) {
        const RID item = rendering->canvas_item_create();
        rendering->canvas_item_set_parent(item, get_canvas_item());
        // Faceplate UVs repeat (UI-L2); the shell is scaled, so sample linearly.
        rendering->canvas_item_set_default_texture_repeat(item, RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
        rendering->canvas_item_set_default_texture_filter(item, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR);
        if (mesh.blend == model::ShellBlend::additive) {
            if (additive_.is_null()) {
                Ref<CanvasItemMaterial> material;
                material.instantiate();
                material->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
                additive_ = material;
            }
            rendering->canvas_item_set_material(item, additive_->get_rid());
        }
        items_.push_back(item);
    }
    panel_item_ = rendering->canvas_item_create();
    rendering->canvas_item_set_parent(panel_item_, get_canvas_item());
    rendering->canvas_item_set_default_texture_filter(panel_item_, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR);
    options_ = options;
    if (options_ != nullptr) add_child(options_);
    for (const Setup::Placed& placed : setup_.time_buttons) add_child(placed.button);
    laid_out_ = Vector2(-1.0F, -1.0F);
    relayout();
}

void EawrTacticalHud::set_probe(std::function<void()> probe) {
    probe_ = std::move(probe);
    set_process(static_cast<bool>(probe_));
}

void EawrTacticalHud::_notification(const int what) {
    if (what == NOTIFICATION_RESIZED || what == NOTIFICATION_READY || what == NOTIFICATION_ENTER_TREE) relayout();
    if (what == NOTIFICATION_PROCESS && probe_ && laid_out_.x > 0.0F) {
        const std::function<void()> probe = std::move(probe_);
        probe_ = nullptr;
        set_process(false);
        probe();
    }
}

void EawrTacticalHud::_draw() {}

model::ReferenceSpace EawrTacticalHud::space() const {
    const Vector2 size = get_size();
    return model::reference_space(
        {static_cast<std::uint32_t>(std::max(0.0F, size.x)), static_cast<std::uint32_t>(std::max(0.0F, size.y))},
        setup_.rules);
}

model::ShellPlacement EawrTacticalHud::placement() const { return setup_.view.placement(space()); }

bool EawrTacticalHud::hit(const double x, const double y) const {
    return setup_.view.hit_test_screen({x, y}, space());
}

void EawrTacticalHud::relayout() {
    const Vector2 size = get_size();
    // Nothing to lay out before setup() has made the canvas items.
    if (size.x <= 0.0F || size.y <= 0.0F || size == laid_out_ || !panel_item_.is_valid()
        || items_.size() != setup_.meshes.size()) {
        return;
    }
    laid_out_ = size;
    const model::ReferenceSpace reference = space();
    const model::ShellPlacement shell = setup_.view.placement(reference);
    RenderingServer* rendering = RenderingServer::get_singleton();
    for (std::size_t index = 0; index < setup_.meshes.size(); ++index) {
        const Mesh& mesh = setup_.meshes[index];
        const RID item = items_[index];
        rendering->canvas_item_clear(item);
        if (mesh.texture.is_null()) continue;
        PackedVector2Array points;
        PackedVector2Array uvs;
        PackedColorArray colours;
        PackedInt32Array indices;
        for (const data::ui::ShellTriangle& triangle : mesh.triangles) {
            for (const data::ui::ShellVertex& vertex : triangle.vertices) {
                const model::ReferencePoint point = model::shell_point_to_screen(vertex.position.x, vertex.position.y, shell);
                indices.push_back(static_cast<int32_t>(points.size()));
                points.push_back(Vector2(static_cast<float>(point.x), static_cast<float>(point.y)));
                uvs.push_back(Vector2(vertex.uv.x, vertex.uv.y));
                colours.push_back(Color(1, 1, 1, 1));
            }
        }
        rendering->canvas_item_add_triangle_array(item, indices, points, colours, uvs, PackedInt32Array(),
                                                  PackedFloat32Array(), mesh.texture->get_rid());
    }
    rendering->canvas_item_clear(panel_item_);
    for (const Setup::Art& art : setup_.panel) {
        if (art.texture.is_null()) continue;
        rendering->canvas_item_add_texture_rect(panel_item_, rect2(model::shell_to_screen(art.quad, shell)),
                                                art.texture->get_rid());
    }
    if (options_ != nullptr && setup_.options_quad) {
        const Rect2 rect = rect2(model::shell_to_screen(*setup_.options_quad, shell));
        options_->set_position(rect.position);
        options_->set_size(rect.size);
        if (auto* button = Object::cast_to<EawrHudButton>(options_); button != nullptr && setup_.options_hit) {
            const Rect2 hit = rect2(model::shell_to_screen(*setup_.options_hit, shell));
            button->set_hit_rect(Rect2(hit.position - rect.position, hit.size));
        }
    }
    for (const Setup::Placed& placed : setup_.time_buttons) {
        const Rect2 rect = rect2(model::shell_to_screen(placed.quad, shell));
        placed.button->set_position(rect.position);
        placed.button->set_size(rect.size);
        if (auto* button = Object::cast_to<EawrHudButton>(placed.button); button != nullptr) {
            button->set_order_shift_pixels(static_cast<float>(shell.scale));
            const Rect2 hit = rect2(model::shell_to_screen(placed.hit, shell));
            button->set_hit_rect(Rect2(hit.position - rect.position, hit.size));
        }
    }
    if (setup_.planet) {
        planet_rect_ = rect2(model::shell_to_screen(setup_.planet->rect, shell));
        const model::FontPixels pixels = model::font_pixels({setup_.planet->point_size, false, 1.0},
                                                            model::font_screen_height(reference));
        planet_pixels_ = pixels.glyph_height;
        KitTextStyle style;
        style.font = setup_.planet_font;
        style.size = pixels.glyph_height;
        style.top = style.bottom = colour(setup_.planet->colour);
        style.emboss = setup_.planet->emboss;
        style.outline = setup_.planet->outline;
        if (setup_.planet_cell) {
            const model::TextCell cell = setup_.planet_cell(pixels.glyph_height);
            style.cell_ascent = cell.ascent;
            style.cell_descent = cell.descent;
        }
        planet_.draw(*this, style, setup_.planet_text, planet_rect_, HORIZONTAL_ALIGNMENT_CENTER, true, false,
                     static_cast<float>(shell.scale), 1.0F);
    }
}


} // namespace eawr::presentation::godot_backend
