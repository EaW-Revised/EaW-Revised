#include "production_view.hpp"

#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <algorithm>
#include <sstream>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

constexpr const char* fallback_icon = "i_button_temporary.tga";
// PU-67: the pane's close button moves up 46 shell units per row the pane does not show.
constexpr float pane_row_height = 46.0F;
// PU-67: the pane's shell origin is the screen's top left, 100 shell units down (unverified
// against the retail still; FoC places it at (-width / 2, height / 2 - 100) in its UI camera).
constexpr double pane_top_offset = 100.0;

[[nodiscard]] Rect2 rect2(const model::PixelRect& rect) {
    return Rect2(static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
                 static_cast<float>(rect.height));
}

[[nodiscard]] std::string rect_json(const Rect2& rect) {
    std::ostringstream output;
    output << "[" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]";
    return output.str();
}

[[nodiscard]] std::string json(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

[[nodiscard]] Color colour(const data::ui::Rgba8& rgba) {
    return Color(rgba.r / 255.0F, rgba.g / 255.0F, rgba.b / 255.0F, rgba.a / 255.0F);
}

[[nodiscard]] std::string numbered(const std::size_t value) {
    return (value < 10 ? "0" : "") + std::to_string(value);
}

} // namespace

EawrProductionPanel::EawrProductionPanel() {
    set_mouse_filter(MOUSE_FILTER_STOP);
    set_focus_mode(FOCUS_NONE);
    set_anchors_and_offsets_preset(PRESET_FULL_RECT);
}

EawrProductionPanel::~EawrProductionPanel() = default;

void EawrProductionPanel::setup(Setup setup) {
    setup_ = std::move(setup);
    queue_texts_.clear();
    for (std::size_t index = 0; index < setup_.queue.size(); ++index) queue_texts_.push_back(std::make_unique<KitText>());
    pool_texts_.clear();
    const std::size_t slots = setup_.pane ? setup_.pane->slots.size() : 0U;
    for (std::size_t index = 0; index < slots; ++index) pool_texts_.push_back(std::make_unique<KitText>());
    set_process(true);
    update_text();
    queue_redraw();
}

void EawrProductionPanel::show(View view) {
    if (view == view_) return;
    view_ = std::move(view);
    if (!view_.reinforcement_allowed) {
        set_pane_open(false);
        dragging_pool_ = false;
    }
    update_text();
    queue_redraw();
}

void EawrProductionPanel::set_pane_open(const bool open) {
    if (open && !view_.reinforcement_allowed) return; // WR-07
    if (open == pane_open_) return;
    pane_open_ = open;
    ++toggles_;
    update_text();
    queue_redraw();
}

Rect2 EawrProductionPanel::screen(const data::ui::ReferenceRect& rect) const {
    return rect2(model::shell_to_screen(rect, setup_.placement()));
}

model::ShellPlacement EawrProductionPanel::pane_placement() const {
    const model::ShellPlacement shell = setup_.placement();
    return {0.0, pane_top_offset * shell.scale, shell.scale};
}

Rect2 EawrProductionPanel::pane_screen(const data::ui::ReferenceRect& rect) const {
    return rect2(model::shell_to_screen(rect, pane_placement()));
}

data::ui::ReferenceRect EawrProductionPanel::close_rect() const {
    data::ui::ReferenceRect rect = setup_.pane->close->rect;
    rect.y += static_cast<float>(model::pool_row_limit - 1 - view_.rows) * pane_row_height;
    return rect;
}

Ref<Texture2D> EawrProductionPanel::icon_texture(const std::string& type, std::string* name) const {
    std::string icon = setup_.icon ? setup_.icon(type) : std::string();
    if (icon.empty()) icon = fallback_icon;
    if (name != nullptr) *name = icon;
    return setup_.texture ? setup_.texture(icon) : Ref<Texture2D>();
}

std::optional<EawrProductionPanel::Target> EawrProductionPanel::target_at(const Vector2& point) const {
    if (!setup_.placement) return std::nullopt;
    if (pane_open_ && setup_.pane) {
        for (const PoolView& slot : view_.pool) {
            if (slot.slot < setup_.pane->slots.size() && pane_screen(setup_.pane->slots[slot.slot].rect).has_point(point)) {
                return Target{Hit::pool, slot.slot};
            }
        }
        if (setup_.pane->close && pane_screen(close_rect()).has_point(point)) return Target{Hit::close, 0};
    }
    for (const QueueView& queued : view_.queue) {
        if (queued.component < setup_.queue.size() && screen(setup_.queue[queued.component].button.rect).has_point(point)) {
            return Target{Hit::queue, queued.component};
        }
    }
    if (setup_.reinforce && screen(setup_.reinforce->rect).has_point(point)) return Target{Hit::reinforce, 0};
    return std::nullopt;
}

bool EawrProductionPanel::_has_point(const Vector2& point) const { return target_at(point).has_value(); }

void EawrProductionPanel::_gui_input(const Ref<InputEvent>& event) {
    // WR-11..15: GUI retains the pressed pool slot's drag even outside its rectangle.
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr()); motion && dragging_pool_) {
        accept_event();
        if (drag_move_) drag_move_(motion->get_global_position());
        return;
    }
    const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr());
    if (button == nullptr || button->get_button_index() != MOUSE_BUTTON_LEFT) return;
    accept_event();
    const auto target = target_at(button->get_position());
    if (button->is_pressed()) {
        pressed_ = target;
        // PU-68: a press on a pool slot starts placing its type (FoC drags from the press).
        if (target && target->kind == Hit::pool) {
            const auto slot = std::find_if(view_.pool.begin(), view_.pool.end(),
                [&](const PoolView& shown) { return shown.slot == target->index; });
            if (slot != view_.pool.end() && slot->enabled && view_.reinforcement_allowed && pick_) {
                ++picks_;
                pick_(target->index);
                dragging_pool_ = true;
                if (drag_move_) drag_move_(button->get_global_position());
            }
        }
        return;
    }
    const auto pressed = pressed_;
    pressed_.reset();
    if (dragging_pool_) {
        dragging_pool_ = false;
        if (drag_drop_) drag_drop_(button->get_global_position());
        return;
    }
    if (!target || !pressed || pressed->kind != target->kind || pressed->index != target->index) return;
    switch (target->kind) {
    case Hit::queue:
        ++cancels_;
        if (cancel_) cancel_(target->index);
        break;
    case Hit::reinforce: set_pane_open(!pane_open_); break;
    case Hit::close: set_pane_open(false); break;
    case Hit::pool: break;
    }
}

void EawrProductionPanel::draw_text(KitText& text, const String& value, const Rect2& box, const HorizontalAlignment align,
                                    const data::ui::Rgba8& rgba, const std::int32_t points, const bool outline) {
    if (!setup_.space || !setup_.placement) return;
    const model::FontPixels pixels = model::font_pixels({points, false, 1.0}, model::font_screen_height(setup_.space()));
    KitTextStyle style;
    style.font = setup_.font;
    style.size = pixels.glyph_height;
    style.top = style.bottom = colour(rgba);
    style.outline = outline;
    if (setup_.cell) {
        const model::TextCell cell = setup_.cell(pixels.glyph_height);
        style.cell_ascent = cell.ascent;
        style.cell_descent = cell.descent;
    }
    text.draw(*this, style, value, box, align, true, false, static_cast<float>(setup_.placement().scale), 1.0F);
}

void EawrProductionPanel::update_text() {
    if (!setup_.placement || !setup_.space) return;
    // PU-64: the front's percentage on the queue's text slot, at the bone plus Text_Offset.
    for (std::size_t component = 0; component < setup_.queue.size(); ++component) {
        const model::HudQueueSlot& look = setup_.queue[component];
        const auto queued = std::find_if(view_.queue.begin(), view_.queue.end(),
            [component](const QueueView& shown) { return shown.component == component; });
        String value;
        if (queued != view_.queue.end() && queued->percent && look.text) value = String(queued->percent->c_str());
        const assets::Vec2f at{look.button.origin.x + look.text_offset.x, look.button.origin.y + look.text_offset.y};
        draw_text(*queue_texts_[component], value, screen({at.x - 25.0F, at.y - 6.0F, 50.0F, 12.0F}),
                  HORIZONTAL_ALIGNMENT_CENTER, look.colour, look.point_size, look.outline);
    }
    // PU-65: the whole credits, right-justified at the bone plus Text_Offset.
    if (setup_.credits) {
        const model::HudIconText& look = *setup_.credits;
        const float centre_y = look.text.rect.y + look.text.rect.height / 2.0F;
        const float right = look.text.rect.x + look.text.rect.width + look.text_offset.x;
        const String value = view_.credits ? String(view_.credits->c_str()) : String();
        draw_text(credits_text_, value, screen({right - 120.0F, centre_y - 8.0F + look.text_offset.y, 120.0F, 16.0F}),
                  look.right_justified ? HORIZONTAL_ALIGNMENT_RIGHT : HORIZONTAL_ALIGNMENT_LEFT, look.text.colour,
                  look.text.point_size, look.text.outline);
    }
    // PU-66: the pool slots' "x<n>" and the population, on the open pane only.
    if (setup_.pane) {
        const model::HudReinforcePane& pane = *setup_.pane;
        for (std::size_t slot = 0; slot < pane.slots.size(); ++slot) {
            const auto shown = std::find_if(view_.pool.begin(), view_.pool.end(),
                [slot](const PoolView& view) { return view.slot == slot; });
            String value;
            if (pane_open_ && shown != view_.pool.end()) value = String(shown->text.c_str());
            const auto& look = pane.slots[slot];
            const assets::Vec2f at{look.origin.x + pane.slot_text_offset.x, look.origin.y + pane.slot_text_offset.y};
            const model::PixelRect box = model::shell_to_screen({at.x - 20.0F, at.y - 6.0F, 40.0F, 12.0F}, pane_placement());
            draw_text(*pool_texts_[slot], value, rect2(box), HORIZONTAL_ALIGNMENT_CENTER, pane.slot_colour,
                      pane.slot_point_size, true);
        }
        if (pane.population) {
            const auto& look = *pane.population;
            const String value = pane_open_ ? String(view_.population.c_str()) : String();
            const float x = look.text.rect.x + look.text.rect.width + look.text_offset.x;
            const float y = look.text.rect.y + look.text.rect.height / 2.0F + look.text_offset.y;
            draw_text(population_text_, value, rect2(model::shell_to_screen({x, y - 6.0F, 60.0F, 12.0F}, pane_placement())),
                      HORIZONTAL_ALIGNMENT_LEFT, look.text.colour, look.text.point_size, look.text.outline);
        }
    }
}

void EawrProductionPanel::_notification(const int what) {
    if (what != NOTIFICATION_PROCESS || !setup_.placement) return;
    const Vector2 size = get_size();
    if (size != laid_out_) {
        laid_out_ = size;
        update_text();
        queue_redraw();
    }
}

void EawrProductionPanel::_draw() {
    if (!setup_.placement || !setup_.texture) return;
    // PU-63: the queued types' icons at texture size around the bone, tinted.
    for (const QueueView& queued : view_.queue) {
        if (queued.component >= setup_.queue.size()) continue;
        const model::HudQueueSlot& look = setup_.queue[queued.component];
        const Ref<Texture2D> texture = icon_texture(queued.type);
        if (texture.is_null()) continue;
        const Vector2 size = texture->get_size();
        const Rect2 quad = screen(model::button_quad(look.button, size.x, size.y));
        draw_texture_rect(texture, quad, false, colour(setup_.queue_tint));
        // PU-64: the front's build progress (unverified look: the build art over the part not
        // yet built, shrinking from the top).
        if (queued.progress < 1.0 && !look.build.empty()) {
            const Ref<Texture2D> build = setup_.texture(look.build);
            if (build.is_valid()) {
                Rect2 left = quad;
                left.size.y = quad.size.y * static_cast<float>(1.0 - queued.progress);
                draw_texture_rect(build, left, false, Color(1, 1, 1, 0.5F));
            }
        }
    }
    // PU-65: the money icon.
    if (setup_.credits && !setup_.credits->icon.empty()) {
        const Ref<Texture2D> icon = setup_.optional_texture ? setup_.optional_texture(setup_.credits->icon) : Ref<Texture2D>();
        if (icon.is_valid()) {
            const auto& rect = setup_.credits->text.rect;
            const Vector2 size = icon->get_size();
            const float x = rect.x + rect.width / 2.0F - size.x / 2.0F;
            const float y = rect.y + rect.height / 2.0F - size.y / 2.0F;
            draw_texture_rect(icon, screen({x, y, size.x, size.y}), false);
        }
    }
    // PU-66, PU-67: the open pane: its art, the pool icons (greyed when they do not fit), the close
    // button and the population icon.
    if (!pane_open_ || !setup_.pane) return;
    const model::HudReinforcePane& pane = *setup_.pane;
    const model::ShellPlacement placement = pane_placement();
    for (const model::HudShellMesh& mesh : pane.meshes) {
        const Ref<Texture2D> texture = setup_.texture(mesh.texture);
        if (texture.is_null()) continue;
        for (const data::ui::ShellTriangle& triangle : mesh.triangles) {
            PackedVector2Array points;
            PackedVector2Array uvs;
            PackedColorArray colours;
            for (const auto& vertex : triangle.vertices) {
                const model::ReferencePoint point = model::shell_point_to_screen(vertex.position.x, vertex.position.y, placement);
                points.push_back(Vector2(static_cast<float>(point.x), static_cast<float>(point.y)));
                uvs.push_back(Vector2(vertex.uv.x, vertex.uv.y));
                colours.push_back(Color(1, 1, 1, 1));
            }
            draw_polygon(points, colours, uvs, texture);
        }
    }
    for (const PoolView& slot : view_.pool) {
        if (slot.slot >= pane.slots.size()) continue;
        const Ref<Texture2D> texture = icon_texture(slot.type);
        if (texture.is_null()) continue;
        const Vector2 size = texture->get_size();
        const Rect2 quad = rect2(model::shell_to_screen(model::button_quad(pane.slots[slot.slot], size.x, size.y), placement));
        draw_texture_rect(texture, quad, false, slot.enabled ? Color(1, 1, 1, 1) : Color(0.5F, 0.5F, 0.5F, 1.0F));
    }
    if (pane.close && !pane.close->normal.empty()) {
        const Ref<Texture2D> texture = setup_.texture(pane.close->normal);
        if (texture.is_valid()) {
            model::HudShellButton look = *pane.close;
            look.origin.y += static_cast<float>(model::pool_row_limit - 1 - view_.rows) * pane_row_height;
            const Vector2 size = texture->get_size();
            draw_texture_rect(texture, rect2(model::shell_to_screen(model::button_quad(look, size.x, size.y), placement)), false);
        }
    }
    if (pane.population && !pane.population->icon.empty()) {
        const Ref<Texture2D> icon = setup_.texture(pane.population->icon);
        if (icon.is_valid()) {
            const auto& rect = pane.population->text.rect;
            const Vector2 size = icon->get_size();
            const float x = rect.x + rect.width / 2.0F - size.x / 2.0F;
            const float y = rect.y + rect.height / 2.0F - size.y / 2.0F;
            draw_texture_rect(icon, rect2(model::shell_to_screen({x, y, size.x, size.y}, placement)), false);
        }
    }
}

std::optional<Rect2> EawrProductionPanel::control_rect(const std::string& name) const {
    if (!setup_.placement) return std::nullopt;
    for (std::size_t component = 0; component < setup_.queue.size(); ++component) {
        if (name == "tqueue" + numbered(component)) return screen(setup_.queue[component].button.rect);
    }
    if (setup_.reinforce && name == setup_.reinforce->name) return screen(setup_.reinforce->rect);
    if (setup_.pane && pane_open_) {
        if (setup_.pane->close && name == "r_close") return pane_screen(close_rect());
        for (std::size_t slot = 0; slot < setup_.pane->slots.size(); ++slot) {
            const std::string slot_name = "r_" + numbered(slot / model::pool_columns) + numbered(slot % model::pool_columns);
            if (name == slot_name) return pane_screen(setup_.pane->slots[slot].rect);
        }
    }
    return std::nullopt;
}

std::string EawrProductionPanel::report_json() const {
    std::ostringstream output;
    output << "{\"queue_slots\": " << setup_.queue.size() << ", \"pane_slots\": "
           << (setup_.pane ? setup_.pane->slots.size() : 0U) << ", \"pane_open\": " << (pane_open_ ? "true" : "false")
           << ", \"toggles\": " << toggles_ << ", \"cancels\": " << cancels_ << ", \"picks\": " << picks_
           << ", \"credits\": " << (view_.credits ? json(*view_.credits) : std::string("null"))
           << ", \"population\": " << json(view_.population) << ", \"queue\": [";
    for (std::size_t index = 0; index < view_.queue.size(); ++index) {
        const QueueView& queued = view_.queue[index];
        std::string icon;
        const Ref<Texture2D> texture = icon_texture(queued.type, &icon);
        output << (index ? ", " : "") << "{\"component\": " << queued.component << ", \"type\": " << json(queued.type)
               << ", \"icon\": " << json(icon) << ", \"icon_drawn\": " << (texture.is_valid() ? "true" : "false")
               << ", \"progress\": " << queued.progress << ", \"percent\": "
               << (queued.percent ? json(*queued.percent) : std::string("null"));
        if (const auto rect = control_rect("tqueue" + numbered(queued.component))) output << ", \"rect\": " << rect_json(*rect);
        output << "}";
    }
    output << "], \"pool\": [";
    for (std::size_t index = 0; index < view_.pool.size(); ++index) {
        const PoolView& slot = view_.pool[index];
        output << (index ? ", " : "") << "{\"slot\": " << slot.slot << ", \"type\": " << json(slot.type)
               << ", \"text\": " << json(slot.text) << ", \"enabled\": " << (slot.enabled ? "true" : "false");
        const std::string name = "r_" + numbered(slot.slot / model::pool_columns) + numbered(slot.slot % model::pool_columns);
        if (const auto rect = control_rect(name)) output << ", \"rect\": " << rect_json(*rect);
        output << "}";
    }
    output << "]";
    if (setup_.reinforce) {
        if (const auto rect = control_rect(setup_.reinforce->name)) output << ", \"reinforce_rect\": " << rect_json(*rect);
    }
    if (const auto rect = control_rect("r_close")) output << ", \"close_rect\": " << rect_json(*rect);
    output << "}";
    return output.str();
}

} // namespace eawr::presentation::godot_backend
