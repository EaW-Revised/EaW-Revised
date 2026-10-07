#include "minimap_view.hpp"
#include "eawr/assets/assets.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/presentation/animation/animation.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
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
    for (const auto& marker : order_markers_) {
        if (marker.item.is_valid()) RenderingServer::get_singleton()->free_rid(marker.item);
    }
    if (order_layer_.is_valid()) RenderingServer::get_singleton()->free_rid(order_layer_);
}

void EawrMinimap::prepare_orders(const vfs::Vfs& filesystem) {
    const auto text = [](const data::XmlNode& node) {
        data::tag_trace::used(node);
        const auto first = node.raw_text.find_first_not_of(" \t\r\n");
        return first == std::string::npos ? std::string{}
            : node.raw_text.substr(first, node.raw_text.find_last_not_of(" \t\r\n") - first + 1);
    };
    const auto number = [&text](const data::XmlNode& node) {
        const auto value = text(node);
        double parsed{};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        return result.ec == std::errc{} && std::isfinite(parsed) ? parsed : 0.0;
    };
    constexpr std::array<std::string_view, 3> tags{"GUI_Movement_Click_Radar_Event_Name",
        "GUI_Attack_Movement_Click_Radar_Event_Name", "GUI_Movement_Double_Click_Radar_Event_Name"};
    if (auto constants = data::load_document(filesystem, "data/xml/gameconstants.xml")) {
        for (const auto& child : constants.value().root.children) {
            for (std::size_t index = 0; index < tags.size(); ++index) {
                if (child.name == tags[index]) order_art_[index].name = text(child);
            }
        }
    }
    if (auto radar = data::load_document(filesystem, "data/xml/radarmap.xml")) {
        for (const auto& group : radar.value().root.children) {
            if (group.name == "RadarMapSettings") for (const auto& child : group.children) {
                if (child.name == "Use_Event_System") order_events_enabled_ = text(child) == "Yes";
            }
            if (group.name != "RadarMapEvents") continue;
            for (const auto& event : group.children) {
                const auto name = std::find_if(event.attributes.begin(), event.attributes.end(),
                    [](const auto& attribute) { return attribute.name == "name"; });
                for (auto& art : order_art_) {
                    if (name == event.attributes.end() || name->value != art.name) continue;
                    data::tag_trace::used_attribute(event, "name");
                    for (const auto& child : event.children) {
                        if (child.name == "Event_Model_Name") art.model = text(child);
                        else if (child.name == "Event_Model_Scale") art.scale = number(child);
                        else if (child.name == "Event_Duration") art.duration = number(child);
                        else if (child.name == "Event_Single_Instance") art.singleton = text(child) == "Yes";
                    }
                }
            }
        }
    }
    // OF-04: cache the authored skinned radar animation in normalized map units.
    // These tiny models are sampled once here; gestures and frames reuse their arrays.
    for (auto& art : order_art_) {
        if (art.duration <= 0.0 || art.duration > 10.0 || art.scale <= 0.0 || art.model.empty()) continue;
        auto mesh = assets::load_model(filesystem, "data/art/models/" + art.model);
        if (!mesh) continue;
        const auto dot = art.model.find_last_of('.');
        auto clip = assets::load_animation(filesystem, "data/art/models/" + art.model.substr(0, dot) + "_idle_00.ala");
        auto player = animation::Player::create(mesh.value(), clip ? &clip.value() : nullptr);
        if (!player) continue;
        const auto transform = [](const animation::Matrix& matrix, const assets::Vec3f point) {
            return assets::Vec3f{matrix[0] * point.x + matrix[4] * point.y + matrix[8] * point.z + matrix[12],
                matrix[1] * point.x + matrix[5] * point.y + matrix[9] * point.z + matrix[13],
                matrix[2] * point.x + matrix[6] * point.y + matrix[10] * point.z + matrix[14]};
        };
        const auto count = static_cast<std::size_t>(std::ceil(art.duration * 30.0));
        art.samples.resize(count);
        for (std::size_t frame = 0; frame < count; ++frame) {
            auto pose = player.value().sample({static_cast<float>(frame) / 30.0F});
            if (!pose) continue;
            auto& sample = art.samples[frame];
            for (const auto& part : mesh.value().meshes) for (const auto& submesh : part.submeshes) {
                Color tint(1, 1, 1, 1);
                for (const auto& parameter : submesh.parameters) {
                    if (parameter.name == "BaseTexture" && std::holds_alternative<std::string>(parameter.value)) {
                        const auto& texture = std::get<std::string>(parameter.value);
                        if (art.texture.is_null() && setup_.texture) art.texture = setup_.texture(texture);
                    } else if (parameter.name == "Color" && std::holds_alternative<assets::Vec4f>(parameter.value)) {
                        const auto value = std::get<assets::Vec4f>(parameter.value);
                        // OF-04: the additive material uses Color RGB; its authored W is zero, not opacity.
                        tint = Color(value.x, value.y, value.z, 1.0F);
                    }
                }
                if (!part.visible) continue;
                for (const auto index : submesh.indices) {
                    const auto& vertex = submesh.vertices[index];
                    assets::Vec3f point{};
                    if (submesh.skin_bones.empty()) {
                        if (part.bone < 0 || static_cast<std::size_t>(part.bone) >= pose.value().bones.size()) continue;
                        const auto& bone = pose.value().bones[static_cast<std::size_t>(part.bone)];
                        if (!bone.visible) continue;
                        point = transform(bone.model_asset, vertex.position);
                    } else {
                        for (std::size_t weight = 0; weight < vertex.bone_weights.size(); ++weight) {
                            if (vertex.bone_weights[weight] == 0.0F) continue;
                            const auto palette = vertex.bone_indices[weight];
                            if (palette >= submesh.skin_bones.size()) continue;
                            const auto bone = submesh.skin_bones[palette];
                            if (bone >= pose.value().bones.size()) continue;
                            const auto posed = transform(pose.value().bones[bone].skin_asset, vertex.position);
                            point.x += posed.x * vertex.bone_weights[weight];
                            point.y += posed.y * vertex.bone_weights[weight];
                        }
                    }
                    sample.indices.push_back(static_cast<int32_t>(sample.points.size()));
                    sample.points.push_back(Vector2(point.x, -point.y));
                    sample.uvs.push_back(Vector2(vertex.texcoord[0].x, vertex.texcoord[0].y));
                    sample.colours.push_back(tint);
                }
            }
        }
    }
    order_material_.instantiate();
    order_material_->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
    auto* server = RenderingServer::get_singleton();
    order_layer_ = server->canvas_item_create();
    server->canvas_item_set_parent(order_layer_, get_canvas_item());
    server->canvas_item_set_clip(order_layer_, true);
    for (auto& marker : order_markers_) {
        marker.item = server->canvas_item_create();
        server->canvas_item_set_parent(marker.item, order_layer_);
        server->canvas_item_set_material(marker.item, order_material_->get_rid());
    }
}

void EawrMinimap::move_feedback(const model::MinimapPoint centre, const model::OrderMode mode, const bool double_click) {
    if (!order_events_enabled_) return;
    const std::size_t art = mode == model::OrderMode::attack_move ? 1U : double_click ? 2U : 0U;
    if (order_art_[art].samples.empty() || order_art_[art].texture.is_null()) return;
    auto* server = RenderingServer::get_singleton();
    OrderMarker* slot = nullptr;
    // OF-03: a double click removes the youngest ordinary radar click.
    if (art == 2U) {
        for (auto& marker : order_markers_) {
            if (marker.active && marker.art == 0U && (!slot || marker.born >= slot->born)) slot = &marker;
        }
    }
    if (order_art_[art].singleton) for (auto& marker : order_markers_) {
        if (marker.active && marker.art == art) { slot = &marker; break; }
    }
    if (!slot) for (auto& marker : order_markers_) if (!marker.active) { slot = &marker; break; }
    if (!slot) slot = &*std::min_element(order_markers_.begin(), order_markers_.end(),
        [](const auto& left, const auto& right) { return left.born < right.born; });
    if (slot->active) ++orders_replaced_;
    server->canvas_item_clear(slot->item);
    slot->centre = centre;
    slot->born = order_seconds_;
    slot->art = art;
    slot->sample = std::numeric_limits<std::size_t>::max();
    slot->active = true;
    ++orders_shown_;
    order_frame(order_seconds_);
}

void EawrMinimap::order_frame(const double seconds) {
    if (!std::isfinite(seconds)) return;
    order_seconds_ = seconds;
    auto* server = RenderingServer::get_singleton();
    const Rect2 rect = minimap_rect();
    if (order_layer_.is_valid()) server->canvas_item_set_custom_rect(order_layer_, true, rect);
    for (auto& marker : order_markers_) {
        if (!marker.active) continue;
        const auto& art = order_art_[marker.art];
        const double age = std::max(0.0, seconds - marker.born);
        if (age >= art.duration) {
            marker.active = false;
            server->canvas_item_clear(marker.item);
            ++orders_expired_;
            continue;
        }
        server->canvas_item_set_transform(marker.item, Transform2D(0.0F,
            Vector2(rect.size.x * static_cast<float>(art.scale), rect.size.y * static_cast<float>(art.scale)),
            0.0F, to_screen(marker.centre)));
        const auto frame = std::min(art.samples.size() - 1, static_cast<std::size_t>(age * 30.0));
        if (frame == marker.sample) continue;
        marker.sample = frame;
        const auto& sample = art.samples[frame];
        server->canvas_item_clear(marker.item);
        if (!sample.indices.is_empty()) server->canvas_item_add_triangle_array(marker.item, sample.indices,
            sample.points, sample.colours, sample.uvs, order_empty_bones_, order_empty_weights_, art.texture->get_rid());
    }
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

void EawrMinimap::set_hazards(const std::span<const model::MinimapHazard> hazards,
    const model::MinimapExtents& extents, const model::MinimapSettings& settings) {
    const auto size = pixel_size();
    const std::array<double, 4> bounds{extents.min_x, extents.min_y, extents.max_x, extents.max_y};
    if (hazards_.is_valid() && size == hazard_size_ && bounds == hazard_extents_
        && std::equal(hazards.begin(), hazards.end(), hazard_inputs_.begin(), hazard_inputs_.end())) return;
    const auto texels = model::minimap_hazards(hazards, extents, settings, size[0], size[1]);
    if (texels.empty()) return; // keep dirty inputs until a drawable texture can be built
    PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(texels.size()));
    std::memcpy(bytes.ptrw(), texels.data(), texels.size());
    const auto image = Image::create_from_data(static_cast<int32_t>(size[0]), static_cast<int32_t>(size[1]), false, Image::FORMAT_RGBA8, bytes);
    if (image.is_null() || image->is_empty()) return;
    hazards_ = ImageTexture::create_from_image(image);
    if (hazards_.is_null()) return;
    hazard_inputs_.assign(hazards.begin(), hazards.end());
    hazard_extents_ = bounds;
    hazard_size_ = size;
    hazard_pixels_ = 0;
    for (std::size_t pixel = 3; pixel < texels.size(); pixel += 4) if (texels[pixel] != 0) ++hazard_pixels_;
    queue_redraw();
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
            // OF-03: double-click belongs to the press; dispatch the order on release.
            right_double_click_ = button->is_double_click();
        } else if (right_down_ && _has_point(at)) {
            right_down_ = false;
            ++moves_;
            const model::MinimapPoint point = to_minimap(at);
            std::ostringstream line;
            line << "move " << point.x << "," << point.y;
            log_.push_back(line.str());
            if (log_.size() > 16) log_.erase(log_.begin());
            if (move_) move_(point, right_double_click_);
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
    if (hazards_.is_valid()) server->canvas_item_add_texture_rect(layers_, rect, hazards_->get_rid());
    if (fog_.is_valid()) server->canvas_item_add_texture_rect(layers_, rect, fog_->get_rid());
    if (backdrop_tiles_.is_valid()) {
        const Vector2 tile = backdrop_tiles_->get_size();
        const float repeats = static_cast<float>(model::minimap_backdrop_repeats);
        server->canvas_item_add_texture_rect_region(layers_, rect, backdrop_tiles_->get_rid(),
                                                    Rect2(0.0F, 0.0F, tile.x * repeats, tile.y * repeats));
    }
    // MM-13/MM-16: points precede every textured icon. Their texture coordinates are truncated
    // at the rounded-up radar resolution; they retain pixel size when the world camera zooms.
    const auto point_width = static_cast<std::uint32_t>(std::ceil(rect.size.x));
    const auto point_height = static_cast<std::uint32_t>(std::ceil(rect.size.y));
    for (auto blip = frame_.blips.rbegin(); blip != frame_.blips.rend(); ++blip) {
        if (const auto pixels = model::minimap_point_pixels(*blip, point_width, point_height)) {
            const float sx = rect.size.x / static_cast<float>(point_width);
            const float sy = rect.size.y / static_cast<float>(point_height);
            draw_rect(Rect2(rect.position.x + static_cast<float>(pixels->x) * sx,
                rect.position.y + static_cast<float>(pixels->y) * sy,
                static_cast<float>(pixels->width) * sx, static_cast<float>(pixels->height) * sy), colour(blip->colour));
        }
    }
    std::size_t missing = 0;
    for (const model::MinimapBlip& blip : frame_.blips) {
        if (blip.icon.empty()) continue;
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
    // SND-40: the authored marker keeps its texture colour, pulses in alpha
    // and size, and stays at the notification position for four seconds.
    const Ref<Texture2D> warning = setup_.texture && !setup_.warning_icon.empty()
        ? setup_.texture(setup_.warning_icon) : Ref<Texture2D>();
    if (warning.is_valid()) {
        for (const auto& marker : frame_.warnings) {
            if (marker.centre.x < -1.0 || marker.centre.x > 1.0 || marker.centre.y < -1.0 || marker.centre.y > 1.0) continue;
            const double phase = std::fmod(std::max(0.0, marker.age) * 1024.0, 510.0);
            const float alpha = static_cast<float>(std::abs(255.0 - phase));
            const float pulse = alpha / 512.0F + 0.5F;
            const Vector2 size = warning->get_size() * setup_.warning_scale * pulse * static_cast<float>(setup_.placement().scale);
            draw_texture_rect(warning, Rect2(to_screen(marker.centre) - size * 0.5F, size), false,
                Color(1.0F, 1.0F, 1.0F, alpha / 255.0F));
        }
    }
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
           << ", \"order_feedback\": {\"shown\": " << orders_shown_ << ", \"expired\": " << orders_expired_
           << ", \"replaced\": " << orders_replaced_ << ", \"active\": "
           << std::count_if(order_markers_.begin(), order_markers_.end(), [](const auto& marker) { return marker.active; })
           << ", \"markers\": [" << [&] {
                std::ostringstream rows;
                bool first = true;
                for (const auto& marker : order_markers_) {
                    if (!marker.active) continue;
                    const auto& art = order_art_[marker.art];
                    const auto point = to_screen(marker.centre);
                    rows << (first ? "" : ",") << "{\"event\":\"" << art.name << "\",\"duration\":" << art.duration
                        << ",\"age\":" << order_seconds_ - marker.born << ",\"screen\":[" << point.x << ',' << point.y
                        << "],\"indices\":" << (marker.sample < art.samples.size() ? art.samples[marker.sample].indices.size() : 0) << '}';
                    first = false;
                }
                return rows.str();
           }() << "]}"
           << ", \"warning_icon\": \"" << setup_.warning_icon << "\", \"warnings\": " << frame_.warnings.size()
           << ", \"warning_texture_available\": " << (setup_.texture && !setup_.warning_icon.empty()
               && setup_.texture(setup_.warning_icon).is_valid() ? "true" : "false")
           << ", \"hazard_pixels\": " << hazard_pixels_ << ", \"hazards\": [" << [&] {
                  std::ostringstream rows;
                  for (std::size_t index = 0; index < hazard_inputs_.size(); ++index) {
                      const auto& hazard = hazard_inputs_[index];
                      rows << (index ? ", " : "") << "[" << hazard.x << ", " << hazard.y << ", " << hazard.x_extent << ", " << hazard.y_extent
                           << ", " << static_cast<unsigned>(hazard.kind) << "]";
                  }
                  return rows.str();
              }() << "]"
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
               << ", " << at.y << "], \"half_size\": [" << blip.half_size[0] << ", " << blip.half_size[1]
               << "], \"point_pixels\": " << (blip.icon.empty() ? blip.point_pixels : 0U)
               << ", \"remembered\": " << (blip.remembered ? "true" : "false")
               << ", \"rotation\": " << blip.rotation_degrees << ", \"colour\": [" << int(blip.colour.r)
               << ", " << int(blip.colour.g) << ", " << int(blip.colour.b) << ", " << int(blip.colour.a) << "]}";
    }
    output << "], \"log\": [";
    for (std::size_t index = 0; index < log_.size(); ++index) output << (index ? ", " : "") << "\"" << log_[index] << "\"";
    output << "]}";
    return output.str();
}

} // namespace eawr::presentation::godot_backend
