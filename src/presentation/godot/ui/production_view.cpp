#include "production_view.hpp"
#include "production_allocation.hpp"

#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <utility>
#include <cstdlib>
#include <new>

void* operator new(const std::size_t size) {
    if (auto* counter = eawr::presentation::godot_backend::production_allocation::counter) ++*counter;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void* operator new[](const std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;
using PackedVector2Array = production_allocation::PackedBuffer<godot::PackedVector2Array, Vector2>;
using PackedColorArray = production_allocation::PackedBuffer<godot::PackedColorArray, Color>;
using PackedVector3Array = production_allocation::PackedBuffer<godot::PackedVector3Array, Vector3>;
using PackedInt32Array = production_allocation::PackedBuffer<godot::PackedInt32Array, std::int32_t>;

constexpr const char* fallback_icon = "i_button_temporary.tga";
// PU-67: the pane's close button moves up 46 shell units per row the pane does not show.
constexpr float pane_row_height = 46.0F;
// PU-67: the pane's shell origin is the screen's top left, 100 shell units down.
constexpr double pane_top_offset = 100.0;
// PU-71: debug build's default component pulse, used by the pool notification.
constexpr double pulse_seconds = 0.5;
// PU-70: Selected_Alpha fades the pressed overlay for 0.2 seconds.
constexpr double press_seconds = 0.2;

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
    allocation_probe_ = OS::get_singleton()->has_environment("EAWR_HUD_ALLOCATION_TEST");
    set_mouse_filter(MOUSE_FILTER_STOP);
    set_focus_mode(FOCUS_NONE);
    set_anchors_and_offsets_preset(PRESET_FULL_RECT);
}

EawrProductionPanel::~EawrProductionPanel() {
    if (auto* rendering = RenderingServer::get_singleton(); rendering && additive_item_.is_valid()) {
        for (auto& art : button_art_) if (art.item.is_valid()) rendering->free_rid(art.item);
        for (auto& art : dial_art_) if (art.item.is_valid()) rendering->free_rid(art.item);
        rendering->free_rid(button_item_);
        rendering->free_rid(additive_item_);
    }
}

void EawrProductionPanel::setup(Setup setup) {
    setup_ = std::move(setup);
    close_art_ = {};
    if (setup_.pane && setup_.pane->close && setup_.texture) {
        const auto& look = *setup_.pane->close;
        const std::array names{&look.normal, &look.mouse_over, &look.pressed};
        for (std::size_t state = 0; state < names.size(); ++state)
            if (!names[state]->empty()) close_art_[state] = setup_.texture(*names[state]);
    }
    auto* rendering = RenderingServer::get_singleton();
    if (!additive_item_.is_valid()) {
        // Keep the original ordering: base button, additive overlays, then text children.
        button_item_ = rendering->canvas_item_create();
        rendering->canvas_item_set_parent(button_item_, get_canvas_item());
        additive_item_ = rendering->canvas_item_create();
        rendering->canvas_item_set_parent(additive_item_, get_canvas_item());
        Ref<CanvasItemMaterial> material;
        material.instantiate();
        material->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
        additive_material_ = material;
        rendering->canvas_item_set_material(additive_item_, material->get_rid());
    }
    prepare_art();
    queue_texts_.clear();
    for (std::size_t index = 0; index < setup_.queue.size(); ++index) queue_texts_.push_back(std::make_unique<KitText>());
    pool_texts_.clear();
    const std::size_t slots = setup_.pane ? setup_.pane->slots.size() : 0U;
    for (std::size_t index = 0; index < slots; ++index) pool_texts_.push_back(std::make_unique<KitText>());
    set_process(true);
    update_text();
    queue_redraw();
}

void EawrProductionPanel::show(const View& view, const double seconds) {
    const bool was_animating = flash_start_.has_value() || (pressed_seconds_ >= 0.0 && seconds_ - pressed_seconds_ < press_seconds);
    seconds_ = seconds;
    if (view.notifications != view_.notifications) {
        flashes_started_ += view.notifications > view_.notifications ? view.notifications - view_.notifications : 0U;
        flash_start_ = view.notification_seconds;
    }
    if (pane_open_ || !view.reinforcement_allowed || view.pool.empty()) flash_start_.reset();
    if (was_animating || flash_start_ || (pressed_seconds_ >= 0.0 && seconds_ - pressed_seconds_ < press_seconds)) queue_redraw();
    if (view == view_) return;
    for (const auto& queued : view.queue) {
        const auto old = std::find_if(view_.queue.begin(), view_.queue.end(),
            [&](const auto& previous) { return previous.component == queued.component; });
        if (queued.component < queue_icons_.size() && (old == view_.queue.end() || old->type != queued.type))
            queue_icons_[queued.component] = icon_texture(queued.type);
    }
    view_ = view;
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
    hovered_close_ = false;
    if (open) flash_start_.reset();
    if (open && open_) open_(); // WR-07: closing and idempotent opens add no cue.
    ++toggles_;
    update_text();
    queue_redraw();
}

bool EawrProductionPanel::reinforce_enabled() const noexcept {
    return view_.reinforcement_allowed && !view_.pool.empty();
}

double EawrProductionPanel::flash_level() const noexcept {
    if (!flash_start_ || !reinforce_enabled() || pane_open_) return 0.0;
    const double elapsed = seconds_ - *flash_start_;
    if (elapsed < 0.0) return 0.0;
    return 1.0 - std::fmod(elapsed, pulse_seconds) / pulse_seconds;
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
    const auto& button = *setup_.pane->close;
    // PU-72: use the same texture-sized rectangle for drawing and mouse input.
    const Vector2 size = close_art_[0].is_valid() ? close_art_[0]->get_size() : Vector2();
    data::ui::ReferenceRect rect = close_art_[0].is_valid()
        ? model::button_quad(button, size.x, size.y) : button.rect;
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
            return Target{Hit::queue, queued.component, queued.entry_id};
        }
    }
    if (setup_.reinforce && screen(setup_.reinforce->rect).has_point(point)) return Target{Hit::reinforce, 0};
    return std::nullopt;
}

bool EawrProductionPanel::_has_point(const Vector2& point) const { return target_at(point).has_value(); }

void EawrProductionPanel::_gui_input(const Ref<InputEvent>& event) {
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
        const auto target = target_at(motion->get_position());
        const bool hovered = target && target->kind == Hit::reinforce;
        if (hovered != hovered_reinforce_) { hovered_reinforce_ = hovered; queue_redraw(); }
        const bool close = target && target->kind == Hit::close;
        if (close != hovered_close_) { hovered_close_ = close; queue_redraw(); }
    }
    // WR-11..15: GUI retains the pressed pool slot's drag even outside its rectangle.
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr()); motion && dragging_pool_) {
        accept_event();
        if (drag_move_) drag_move_(motion->get_global_position());
        return;
    }
    const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr());
    // WR-X01: Godot retains pool drags in this GUI, including wheel events.
    // Claim the release too, so this gesture cannot also reach camera zoom.
    if (button && dragging_pool_ && (button->get_button_index() == MOUSE_BUTTON_WHEEL_UP ||
                                    button->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)) {
        accept_event();
        if (button->is_pressed() && drag_wheel_) {
            const double direction = button->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? 1.0 : -1.0;
            drag_wheel_(direction * button->get_factor());
        }
        return;
    }
    if (button == nullptr || (button->get_button_index() != MOUSE_BUTTON_LEFT && button->get_button_index() != MOUSE_BUTTON_RIGHT)) return;
    accept_event();
    const auto target = target_at(button->get_position());
    const bool right = button->get_button_index() == MOUSE_BUTTON_RIGHT;
    if (right && dragging_pool_) {
        dragging_pool_ = false;
        pressed_.reset();
        if (drag_cancel_) drag_cancel_();
        return;
    }
    if (button->is_pressed()) {
        pressed_ = target;
        pressed_right_ = right;
        if (target && target->kind == Hit::close && !right) queue_redraw();
        if (target && target->kind == Hit::reinforce && !right && reinforce_enabled()) {
            pressed_seconds_ = seconds_;
            queue_redraw();
        }
        // PU-68: a press on a pool slot starts placing its type (FoC drags from the press).
        if (target && target->kind == Hit::pool && !right) {
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
    if (pressed && pressed->kind == Hit::close) queue_redraw();
    if (pressed_right_ != right) return;
    if (dragging_pool_) {
        dragging_pool_ = false;
        if (drag_drop_) drag_drop_(button->get_global_position());
        return;
    }
    if (!target || !pressed || pressed->kind != target->kind || pressed->index != target->index) return;
    switch (target->kind) {
    case Hit::queue:
        // PU-63: left click leaves the queue alone; right release cancels and refunds.
        if (right) { ++cancels_; if (cancel_) cancel_(pressed->index, pressed->entry_id); }
        break;
    case Hit::reinforce: if (!right && reinforce_enabled()) set_pane_open(!pane_open_); break;
    case Hit::close: if (!right) set_pane_open(false); break;
    case Hit::pool: break;
    }
}

void EawrProductionPanel::draw_text(KitText& text, const String& value, const Rect2& box, const HorizontalAlignment align,
                                    const data::ui::Rgba8& rgba, const std::int32_t points, const bool outline,
                                    const bool emboss, const bool close) {
    if (!setup_.space || !setup_.placement) return;
    const model::FontPixels pixels = model::font_pixels({points, false, 1.0}, model::font_screen_height(setup_.space()));
    KitTextStyle style;
    style.font = close && setup_.close_font.is_valid() ? setup_.close_font : setup_.font;
    style.size = pixels.glyph_height;
    style.top = style.bottom = colour(rgba);
    style.outline = outline;
    style.emboss = emboss;
    const auto& cell_lookup = close && setup_.close_cell ? setup_.close_cell : setup_.cell;
    if (cell_lookup) {
        const model::TextCell cell = cell_lookup(pixels.glyph_height);
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
        if (pane.close && pane.close_text) {
            const auto& label = *pane.close_text;
            auto box = close_rect();
            box.x += label.text_offset.x;
            box.y += label.text_offset.y;
            draw_text(close_text_, pane_open_ ? String(pane.close_label.c_str()) : String(), pane_screen(box),
                      HORIZONTAL_ALIGNMENT_CENTER, label.text.colour, label.text.point_size,
                      label.text.outline, label.text.emboss, true);
        }
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
    if (what == NOTIFICATION_MOUSE_EXIT && hovered_close_) {
        hovered_close_ = false;
        queue_redraw();
    }
    if (what == NOTIFICATION_MOUSE_EXIT && hovered_reinforce_) {
        hovered_reinforce_ = false;
        queue_redraw();
    }
    if (what != NOTIFICATION_PROCESS || !setup_.placement) return;
    if (allocation_probe_) queue_redraw();
    const Vector2 size = get_size();
    if (size != laid_out_) {
        laid_out_ = size;
        update_text();
        queue_redraw();
    }
}

void EawrProductionPanel::prepare_art() {
    auto* rendering = RenderingServer::get_singleton();
    for (auto& art : button_art_) {
        if (art.item.is_valid()) rendering->free_rid(art.item);
        art = {};
    }
    for (auto& art : dial_art_) if (art.item.is_valid()) rendering->free_rid(art.item);
    dial_art_.clear();
    queue_icons_.clear();
    queue_icons_.resize(setup_.queue.size());
    art_placement_.reset();
    if (setup_.reinforce && setup_.texture) {
        const auto& look = *setup_.reinforce;
        const std::array<const std::string*, 6> names{
            &look.blank, &look.normal, &look.disabled, &look.mouse_over, &look.pressed, &look.flash};
        for (std::size_t layer = 0; layer < names.size(); ++layer) {
            if (names[layer]->empty()) continue;
            auto& art = button_art_[layer];
            art.texture = setup_.texture(*names[layer]);
            if (art.texture.is_null()) continue;
            art.item = rendering->canvas_item_create();
            const bool additive = layer == 3 || layer == 5 || (layer == 4 && !look.selected_alpha);
            rendering->canvas_item_set_parent(art.item, additive ? additive_item_ : button_item_);
            if (additive) rendering->canvas_item_set_material(art.item, additive_material_->get_rid());
        }
    }
    // PU-64: immutable fan topology. The vertex shader moves the unfinished rim vertices
    // onto the current endpoint; the remaining triangles have zero area. No packed buffers
    // are mutated after submission (which would copy Godot's shared storage on redraw).
    PackedVector3Array vertices;
    PackedVector2Array uvs;
    PackedInt32Array indices;
    vertices.resize(10);
    uvs.resize(10);
    indices.resize(24);
    vertices.set(0, Vector3(0.5F, 0.5F, 0));
    uvs.set(0, Vector2(-1, 0));
    static const std::array<Vector2, 9> rim{
        Vector2(0.5F, 0), Vector2(1, 0), Vector2(1, 0.5F), Vector2(1, 1), Vector2(0.5F, 1),
        Vector2(0, 1), Vector2(0, 0.5F), Vector2(0, 0), Vector2(0.5F, 0)};
    for (int index = 0; index < 9; ++index) {
        vertices.set(index + 1, Vector3(rim[static_cast<std::size_t>(index)].x, rim[static_cast<std::size_t>(index)].y, 0));
        uvs.set(index + 1, Vector2(static_cast<float>(index), 0));
        if (index < 8) {
            indices.set(index * 3, 0);
            indices.set(index * 3 + 1, index + 1);
            indices.set(index * 3 + 2, index + 2);
        }
    }
    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    arrays[Mesh::ARRAY_VERTEX] = vertices;
    arrays[Mesh::ARRAY_TEX_UV] = uvs;
    arrays[Mesh::ARRAY_INDEX] = indices;
    dial_mesh_.instantiate();
    dial_mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
    Ref<Shader> shader;
    shader.instantiate();
    shader->set_code(R"(shader_type canvas_item;
render_mode blend_add;
uniform float completion = 0.0;
void vertex() {
    vec2 rim[9] = vec2[9](vec2(0.5,0.0),vec2(1.0,0.0),vec2(1.0,0.5),vec2(1.0,1.0),
        vec2(0.5,1.0),vec2(0.0,1.0),vec2(0.0,0.5),vec2(0.0,0.0),vec2(0.5,0.0));
    float step = min(UV.x, clamp(completion, 0.0, 1.0) * 8.0);
    int segment = int(floor(max(step, 0.0)));
    UV = UV.x < 0.0 ? vec2(0.5) : mix(rim[segment], rim[min(segment+1,8)], fract(step));
    VERTEX = UV;
}
)");
    dial_art_.resize(setup_.queue.size());
    for (std::size_t index = 0; index < dial_art_.size(); ++index) {
        auto& art = dial_art_[index];
        if (setup_.queue[index].build.empty() || !setup_.texture) continue;
        art.texture = setup_.texture(setup_.queue[index].build);
        if (art.texture.is_null()) continue;
        art.material.instantiate();
        art.material->set_shader(shader);
        art.item = rendering->canvas_item_create();
        rendering->canvas_item_set_parent(art.item, additive_item_);
        rendering->canvas_item_set_material(art.item, art.material->get_rid());
        rendering->canvas_item_add_mesh(art.item, dial_mesh_->get_rid(), Transform2D(), Color(1,1,1,1), art.texture->get_rid());
        rendering->canvas_item_set_visible(art.item, false);
    }
}

void EawrProductionPanel::place_art() {
    const auto placement = setup_.placement();
    if (art_placement_ && *art_placement_ == placement) return;
    art_placement_ = placement;
    ++art_builds_;
    auto* rendering = RenderingServer::get_singleton();
    if (setup_.reinforce) {
        for (auto& art : button_art_) {
            if (!art.item.is_valid()) continue;
            const auto size = art.texture->get_size();
            rendering->canvas_item_clear(art.item);
            rendering->canvas_item_add_texture_rect(art.item,
                screen(model::button_quad(*setup_.reinforce, size.x, size.y)), art.texture->get_rid());
        }
    }
    for (std::size_t index = 0; index < dial_art_.size(); ++index) {
        auto& art = dial_art_[index];
        if (!art.item.is_valid()) continue;
        const auto size = art.texture->get_size();
        const auto quad = screen(model::button_quad(setup_.queue[index].button, size.x, size.y));
        rendering->canvas_item_set_transform(art.item,
            Transform2D(Vector2(quad.size.x, 0), Vector2(0, quad.size.y), quad.position));
    }
}

void EawrProductionPanel::show_button(const std::size_t layer, const bool visible, const float level, const bool shifted) {
    const auto& art = button_art_[layer];
    if (!art.item.is_valid()) return;
    auto* rendering = RenderingServer::get_singleton();
    rendering->canvas_item_set_visible(art.item, visible);
    rendering->canvas_item_set_modulate(art.item, Color(level, level, level, 1));
    const auto shift = shifted ? static_cast<float>(art_placement_->scale) : 0.0F;
    rendering->canvas_item_set_transform(art.item, Transform2D(0, Vector2(shift, shift)));
}

void EawrProductionPanel::draw_dial(const std::size_t component, const double completion) {
    auto& art = dial_art_[component];
    if (!art.item.is_valid() || completion <= 0.0) return;
    art.material->set_shader_parameter(completion_parameter_, completion);
    RenderingServer::get_singleton()->canvas_item_set_visible(art.item, true);
    ++dials_drawn_;
}

void EawrProductionPanel::_draw() {
    if (!setup_.placement || !setup_.texture) return;
    close_art_rect_.reset();
    RenderingServer::get_singleton()->canvas_item_clear(additive_item_);
    reinforce_drawn_ = flash_drawn_ = false;
    dials_drawn_ = 0;
    place_art();
    ++art_frames_;
    std::size_t allocations{};
    std::size_t buffers{};
    {
        production_allocation::Scope allocation_scope(allocations, buffers);
        for (const auto& art : dial_art_) if (art.item.is_valid()) RenderingServer::get_singleton()->canvas_item_set_visible(art.item, false);
        // PU-70: always-visible layered button at the shell's b_reinforcement bone.
        if (setup_.reinforce) {
            const auto& look = *setup_.reinforce;
            const auto press_level = 1.0 - (seconds_ - pressed_seconds_) / press_seconds;
            const bool enabled = reinforce_enabled();
            const bool pressed = enabled && press_level > 0.0 && press_level <= 1.0;
            const bool shifted = pressed && look.click_shift;
            const auto level = flash_level();
            show_button(0, true, 1, shifted);
            show_button(1, true, 1, shifted);
            show_button(2, !enabled, 1, shifted);
            show_button(3, enabled && hovered_reinforce_ && !pressed, 1, shifted);
            show_button(4, pressed, static_cast<float>(press_level), shifted);
            show_button(5, level > 0.0, static_cast<float>(level), shifted);
            reinforce_drawn_ = button_art_[1].texture.is_valid();
            flash_drawn_ = level > 0.0 && button_art_[5].texture.is_valid();
            if (flash_drawn_) ++flash_frames_;
        }
        // PU-63: the queued types' icons at texture size around the bone, tinted.
        for (const QueueView& queued : view_.queue) {
            if (queued.component >= setup_.queue.size()) continue;
            const model::HudQueueSlot& look = setup_.queue[queued.component];
            const Ref<Texture2D>& texture = queue_icons_[queued.component];
            if (texture.is_null()) continue;
            const Vector2 size = texture->get_size();
            const Rect2 quad = screen(model::button_quad(look.button, size.x, size.y));
            draw_texture_rect(texture, quad, false, colour(setup_.queue_tint));
            // PU-64: only a front entry has percentage text and a growing clock dial.
            if (queued.percent && !look.build.empty()) draw_dial(queued.component, queued.progress);
        }
    }
    if (art_frames_ > 1) {
        ++warmed_art_frames_;
        art_allocations_ += allocations;
        art_buffer_work_ += buffers;
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
    for (const model::HudShellMesh& mesh : pane.meshes[view_.rows]) {
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
        // PU-72: this component swaps its authored normal, hover and pressed art.
        const bool pressed = pressed_ && pressed_->kind == Hit::close && !pressed_right_;
        const std::size_t state = pressed ? 2U : hovered_close_ ? 1U : 0U;
        const Ref<Texture2D>& texture = close_art_[state].is_valid() ? close_art_[state] : close_art_[0];
        if (texture.is_valid()) {
            model::HudShellButton look = *pane.close;
            look.origin.y += static_cast<float>(model::pool_row_limit - 1 - view_.rows) * pane_row_height;
            const Vector2 size = texture->get_size();
            const Rect2 quad = rect2(model::shell_to_screen(model::button_quad(look, size.x, size.y), placement));
            close_art_rect_ = quad;
            if (state != 0 && !pane.close_swap_texture && close_art_[0].is_valid()) {
                const Vector2 normal_size = close_art_[0]->get_size();
                draw_texture_rect(close_art_[0], rect2(model::shell_to_screen(
                    model::button_quad(look, normal_size.x, normal_size.y), placement)), false);
                RenderingServer::get_singleton()->canvas_item_add_texture_rect(additive_item_, quad, texture->get_rid());
            } else draw_texture_rect(texture, quad, false);
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
           << ", \"reinforce_drawn\": " << (reinforce_drawn_ ? "true" : "false")
           << ", \"reinforce_enabled\": " << (reinforce_enabled() ? "true" : "false")
           << ", \"flash_drawn\": " << (flash_drawn_ ? "true" : "false")
           << ", \"flash_continuous\": true, \"flash_period_seconds\": " << pulse_seconds
           << ", \"flashes_started\": " << flashes_started_
           << ", \"flash_frames\": " << flash_frames_ << ", \"dials_drawn\": " << dials_drawn_
           << ", \"flash_level\": " << flash_level()
           << ", \"art_builds\": " << art_builds_ << ", \"art_frames\": " << art_frames_
           << ", \"warmed_art_frames\": " << warmed_art_frames_ << ", \"art_allocations\": " << art_allocations_
           << ", \"art_buffer_work\": " << art_buffer_work_
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
    if (setup_.pane) {
        output << ", \"pane_rows\": " << view_.rows + 1
               << ", \"close_text_key\": " << json(setup_.pane->close_text_key)
               << ", \"close_label\": " << json(setup_.pane->close_label);
        output << ", \"pane_meshes\": [";
        const auto& meshes = setup_.pane->meshes[view_.rows];
        for (std::size_t index = 0; index < meshes.size(); ++index)
            output << (index ? ", " : "") << json(meshes[index].name);
        output << "]";
    }
    if (const auto rect = control_rect("r_close")) {
        output << ", \"close_rect\": " << rect_json(*rect);
    }
    if (close_art_rect_) output << ", \"close_art_rect\": " << rect_json(*close_art_rect_);
    output << "}";
    return output.str();
}

} // namespace eawr::presentation::godot_backend
