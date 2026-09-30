#include "minimap_view.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <sstream>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

[[nodiscard]] std::string rect_json(const Rect2& rect) {
    std::ostringstream output;
    output << "[" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]";
    return output.str();
}

[[nodiscard]] Color colour(const data::ui::Rgba8 value) {
    return Color(value.r / 255.0F, value.g / 255.0F, value.b / 255.0F, value.a / 255.0F);
}

} // namespace

EawrMinimap::EawrMinimap() {
    set_mouse_filter(MOUSE_FILTER_STOP);
    set_focus_mode(FOCUS_NONE);
    set_anchors_and_offsets_preset(PRESET_FULL_RECT);
}

EawrMinimap::~EawrMinimap() {
    if (layers_.is_valid()) RenderingServer::get_singleton()->free_rid(layers_);
}

void EawrMinimap::setup(Setup setup) {
    setup_ = std::move(setup);
    backdrop_ = setup_.texture && !setup_.backdrop.empty() ? setup_.texture(setup_.backdrop) : Ref<Texture2D>();
    backdrop_tiles_.unref();
    if (backdrop_.is_valid()) {
        // MM-13: the grid tiles 25 times; mipmaps keep its one-texel lines faint instead of aliased.
        const Ref<Image> image = backdrop_->get_image();
        if (image.is_valid() && !image->is_empty()) {
            if (image->is_compressed()) image->decompress();
            if (!image->has_mipmaps()) image->generate_mipmaps();
            backdrop_tiles_ = ImageTexture::create_from_image(image);
        }
    }
    set_process(true);
    queue_redraw();
}

void EawrMinimap::show(Frame frame) {
    frame_ = std::move(frame);
    ++frames_;
    queue_redraw();
}

void EawrMinimap::set_fog(const std::span<const std::uint8_t> texels, const std::uint32_t width,
                          const std::uint32_t height, const std::uint64_t pass) {
    if (width == 0 || height == 0 || texels.size() != static_cast<std::size_t>(width) * height * 4U) return;
    if (fog_.is_valid() && pass == fog_pass_ && width == fog_width_ && height == fog_height_) return;
    PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(texels.size()));
    std::memcpy(bytes.ptrw(), texels.data(), texels.size());
    const Ref<Image> image = Image::create_from_data(static_cast<int32_t>(width), static_cast<int32_t>(height), false,
                                                     Image::FORMAT_RGBA8, bytes);
    if (fog_.is_valid() && width == fog_width_ && height == fog_height_) {
        fog_->update(image);
    } else {
        fog_ = ImageTexture::create_from_image(image);
    }
    fog_pass_ = pass;
    fog_width_ = width;
    fog_height_ = height;
    queue_redraw();
}

Rect2 EawrMinimap::minimap_rect() const {
    if (!setup_.placement) return Rect2();
    const model::PixelRect rect = model::shell_to_screen(setup_.rect, setup_.placement());
    return Rect2(static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
                 static_cast<float>(rect.height));
}

std::array<std::uint32_t, 2> EawrMinimap::pixel_size() const {
    const Rect2 rect = minimap_rect();
    // MM-10: the engine sizes its layers to the radar's screen extent, rounded up.
    return {static_cast<std::uint32_t>(std::max(0.0F, std::ceil(rect.size.x))),
            static_cast<std::uint32_t>(std::max(0.0F, std::ceil(rect.size.y)))};
}

Vector2 EawrMinimap::to_screen(const model::MinimapPoint point) const {
    const Rect2 rect = minimap_rect();
    return Vector2(rect.position.x + static_cast<float>((point.x + 1.0) / 2.0) * rect.size.x,
                   rect.position.y + static_cast<float>((1.0 - point.y) / 2.0) * rect.size.y);
}

model::MinimapPoint EawrMinimap::to_minimap(const Vector2& point) const {
    const Rect2 rect = minimap_rect();
    if (!rect.has_area()) return {};
    return {(point.x - rect.position.x) / rect.size.x * 2.0 - 1.0, 1.0 - (point.y - rect.position.y) / rect.size.y * 2.0};
}

bool EawrMinimap::_has_point(const Vector2& point) const { return minimap_rect().has_point(point); }

void EawrMinimap::look_at(const Vector2& at, const char* why) {
    ++looks_;
    const model::MinimapPoint point = to_minimap(at);
    std::ostringstream line;
    line << why << " " << point.x << "," << point.y;
    log_.push_back(line.str());
    if (log_.size() > 16) log_.erase(log_.begin());
    if (look_) look_(point);
}

void EawrMinimap::_gui_input(const Ref<InputEvent>& event) {
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
        if (!left_press_ || (motion->get_button_mask() & MOUSE_BUTTON_MASK_LEFT) == 0) return;
        accept_event();
        const Vector2 at = motion->get_position();
        // MM-11: the drag follows only while the pointer is on the minimap.
        if (!_has_point(at)) return;
        if (!dragged_) {
            if (at.distance_to(*left_press_) <= static_cast<float>(model::minimap_drag_pixels)) return;
            dragged_ = true;
            ++drags_;
        }
        look_at(at, "drag");
        return;
    }
    const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr());
    if (button == nullptr) return;
    const Vector2 at = button->get_position();
    if (button->get_button_index() == MOUSE_BUTTON_LEFT) {
        accept_event();
        if (button->is_pressed()) {
            // MM-11: the press itself looks at the point.
            left_press_ = at;
            dragged_ = false;
            look_at(at, "press");
        } else {
            left_press_.reset();
            dragged_ = false;
        }
    } else if (button->get_button_index() == MOUSE_BUTTON_RIGHT) {
        accept_event();
        if (button->is_pressed()) {
            right_down_ = true;
        } else if (right_down_ && _has_point(at)) {
            right_down_ = false;
            ++moves_;
            const model::MinimapPoint point = to_minimap(at);
            std::ostringstream line;
            line << "move " << point.x << "," << point.y;
            log_.push_back(line.str());
            if (log_.size() > 16) log_.erase(log_.begin());
            if (move_) move_(point);
        } else {
            right_down_ = false;
        }
    }
}

void EawrMinimap::_notification(const int what) {
    if (what == NOTIFICATION_MOUSE_EXIT) {
        right_down_ = false;
    } else if (what == NOTIFICATION_PROCESS) {
        if (get_size() != laid_out_) {
            laid_out_ = get_size();
            queue_redraw();
        }
    }
}

void EawrMinimap::_draw() {
    const Rect2 rect = minimap_rect();
    if (!rect.has_area()) return;
    // MM-13: background, fog, backdrop, blips, the camera's outline. The first two go on a child canvas item
    // behind this one, which repeats textures.
    RenderingServer* server = RenderingServer::get_singleton();
    if (!layers_.is_valid()) {
        layers_ = server->canvas_item_create();
        server->canvas_item_set_parent(layers_, get_canvas_item());
        server->canvas_item_set_draw_behind_parent(layers_, true);
        server->canvas_item_set_default_texture_filter(layers_, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS);
        server->canvas_item_set_default_texture_repeat(layers_, RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
    }
    server->canvas_item_clear(layers_);
    // MM-14: the background layer, one colour in space.
    server->canvas_item_add_rect(layers_, rect, colour(setup_.background));
    if (fog_.is_valid()) server->canvas_item_add_texture_rect(layers_, rect, fog_->get_rid());
    if (backdrop_tiles_.is_valid()) {
        const Vector2 tile = backdrop_tiles_->get_size();
        const float repeats = static_cast<float>(model::minimap_backdrop_repeats);
        server->canvas_item_add_texture_rect_region(layers_, rect, backdrop_tiles_->get_rid(),
                                                    Rect2(0.0F, 0.0F, tile.x * repeats, tile.y * repeats));
    }
    std::size_t missing = 0;
    for (const model::MinimapBlip& blip : frame_.blips) {
        const Ref<Texture2D> texture = setup_.texture ? setup_.texture(blip.icon) : Ref<Texture2D>();
        if (texture.is_null()) {
            ++missing;
            continue;
        }
        const Vector2 centre = to_screen(blip.centre);
        float half_x = static_cast<float>(blip.half_size[0] / 2.0) * rect.size.x;
        float half_y = static_cast<float>(blip.half_size[1] / 2.0) * rect.size.y;
        // Counter-clockwise in the minimap frame is clockwise on the screen (y down).
        double degrees = -blip.rotation_degrees;
        if (blip.rotate_icon) {
            // Radar_Rotate_Icon turns the texture a quarter inside the quad (unverified direction).
            degrees -= 90.0;
            std::swap(half_x, half_y);
        }
        draw_set_transform(centre, static_cast<float>(degrees * std::numbers::pi / 180.0), Vector2(1.0F, 1.0F));
        draw_texture_rect(texture, Rect2(-half_x, -half_y, half_x * 2.0F, half_y * 2.0F), false, colour(blip.colour));
    }
    draw_set_transform(Vector2(), 0.0F, Vector2(1.0F, 1.0F));
    icons_missing_ = missing;
    if (frame_.guide) {
        // The outline's lines stop at the minimap's edge, as the engine's radar viewport cuts them.
        const auto& corners = *frame_.guide;
        for (std::size_t index = 0; index < corners.size(); ++index) {
            const auto segment = model::minimap_clip(corners[index], corners[(index + 1) % corners.size()]);
            if (!segment) continue;
            draw_line(to_screen((*segment)[0]), to_screen((*segment)[1]), Color(1.0F, 1.0F, 1.0F, 1.0F), 1.0F, false);
        }
    }
}

std::string EawrMinimap::report_json() const {
    std::ostringstream output;
    output << "{\"rect\": " << rect_json(minimap_rect()) << ", \"backdrop\": \"" << setup_.backdrop
           << "\", \"backdrop_drawn\": " << (backdrop_tiles_.is_valid() ? "true" : "false")
           << ", \"backdrop_repeats\": " << model::minimap_backdrop_repeats << ", \"frames\": " << frames_
           << ", \"blips\": " << frame_.blips.size() << ", \"icons_missing\": " << icons_missing_
           << ", \"fog\": {\"width\": " << fog_width_ << ", \"height\": " << fog_height_ << ", \"passes\": " << fog_pass_
           << "}, \"looks\": " << looks_ << ", \"drags\": " << drags_ << ", \"moves\": " << moves_ << ", \"guide\": ";
    if (frame_.guide) {
        output << "[";
        for (std::size_t index = 0; index < frame_.guide->size(); ++index) {
            const Vector2 point = to_screen((*frame_.guide)[index]);
            output << (index ? ", " : "") << "[" << point.x << ", " << point.y << "]";
        }
        output << "]";
    } else {
        output << "null";
    }
    output << ", \"drawn\": [";
    const std::size_t shown = std::min<std::size_t>(frame_.blips.size(), 64);
    for (std::size_t index = 0; index < shown; ++index) {
        const model::MinimapBlip& blip = frame_.blips[index];
        const Vector2 at = to_screen(blip.centre);
        output << (index ? ", " : "") << "{\"id\": " << blip.id << ", \"icon\": \"" << blip.icon << "\", \"at\": [" << at.x
               << ", " << at.y << "], \"rotation\": " << blip.rotation_degrees << ", \"colour\": [" << int(blip.colour.r)
               << ", " << int(blip.colour.g) << ", " << int(blip.colour.b) << ", " << int(blip.colour.a) << "]}";
    }
    output << "], \"log\": [";
    for (std::size_t index = 0; index < log_.size(); ++index) output << (index ? ", " : "") << "\"" << log_[index] << "\"";
    output << "]}";
    return output.str();
}

} // namespace eawr::presentation::godot_backend
