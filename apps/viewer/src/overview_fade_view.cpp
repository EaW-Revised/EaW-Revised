#include "overview_fade_view.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>

#include <sstream>

namespace eawr::presentation::godot_backend {

using namespace godot;

namespace {

// The held image replaces what is under it by its opacity; its own alpha channel is ignored, so a
// render target that keeps alpha below one draws the same.
constexpr const char* fade_shader = R"(shader_type canvas_item;
render_mode blend_mix, unshaded;
uniform float opacity = 0.0;
void fragment() {
	COLOR = vec4(texture(TEXTURE, UV).rgb, opacity);
}
)";

} // namespace

OverviewFadeView::OverviewFadeView(Node& host) {
    // Over the HUD (layer 1) and the battle overlay, below the F3 performance overlay (layer 100),
    // a project tool FoC does not have.
    layer_ = memnew(CanvasLayer);
    layer_->set_name("EawrOverviewFadeLayer");
    layer_->set_layer(99);
    rect_ = memnew(TextureRect);
    rect_->set_name("EawrOverviewFade");
    rect_->set_anchors_preset(Control::PRESET_FULL_RECT);
    rect_->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    rect_->set_stretch_mode(TextureRect::STRETCH_SCALE);
    rect_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    rect_->set_visible(false);
    Ref<Shader> shader;
    shader.instantiate();
    shader->set_code(fade_shader);
    material_.instantiate();
    material_->set_shader(shader);
    rect_->set_material(material_);
    layer_->add_child(rect_);
    host.add_child(layer_);
}

OverviewFadeView::~OverviewFadeView() { release(); }

void OverviewFadeView::release() {
    if (texture_.is_valid()) texture_->set_texture_rd_rid(RID());
    if (held_.is_valid()) {
        RenderingServer* rendering = RenderingServer::get_singleton();
        RenderingDevice* device = rendering != nullptr ? rendering->get_rendering_device() : nullptr;
        if (device != nullptr) device->free_rid(held_);
        held_ = RID();
    }
}

bool OverviewFadeView::hold(Viewport& viewport) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    RenderingDevice* device = rendering != nullptr ? rendering->get_rendering_device() : nullptr;
    if (device == nullptr) {
        capture_ = "unavailable: no RenderingDevice backend";
        return false;
    }
    const Ref<ViewportTexture> shown = viewport.get_texture();
    const RID source = shown.is_valid() ? rendering->texture_get_rd_texture(shown->get_rid()) : RID();
    if (!source.is_valid() || !device->texture_is_valid(source)) {
        capture_ = "unavailable: the viewport has no RenderingDevice texture";
        return false;
    }
    const Ref<RDTextureFormat> format = device->texture_get_format(source);
    if (format.is_null() || (format->get_usage_bits() & RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT) == 0) {
        capture_ = "unavailable: the viewport texture cannot be copied";
        return false;
    }
    const auto width = static_cast<std::int32_t>(format->get_width());
    const auto height = static_cast<std::int32_t>(format->get_height());
    const auto data_format = static_cast<std::int64_t>(format->get_format());
    if (!held_.is_valid() || width != held_width_ || height != held_height_ || data_format != held_format_) {
        release();
        Ref<RDTextureFormat> copy;
        copy.instantiate();
        copy->set_format(format->get_format());
        copy->set_width(static_cast<uint32_t>(width));
        copy->set_height(static_cast<uint32_t>(height));
        copy->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
        Ref<RDTextureView> view;
        view.instantiate();
        held_ = device->texture_create(copy, view);
        if (!held_.is_valid()) {
            capture_ = "unavailable: the held texture was not created";
            return false;
        }
        held_width_ = width;
        held_height_ = height;
        held_format_ = data_format;
        if (texture_.is_null()) texture_.instantiate();
        texture_->set_texture_rd_rid(held_);
        rect_->set_texture(texture_);
    }
    // The render target still holds the last drawn frame: this frame has not drawn yet.
    if (device->texture_copy(source, held_, Vector3(), Vector3(), Vector3(static_cast<float>(width), static_cast<float>(height), 1.0F),
                             0, 0, 0, 0) != OK) {
        capture_ = "unavailable: the copy failed";
        return false;
    }
    capture_ = "gpu copy";
    ++held_images_;
    return true;
}

void OverviewFadeView::request(Viewport& viewport) {
    fade_.request();
    static_cast<void>(hold(viewport));
}

void OverviewFadeView::frame() {
    opacity_ = fade_.next_frame();
    const bool shown = opacity_.has_value() && held_.is_valid() && capture_ == "gpu copy";
    if (shown) material_->set_shader_parameter("opacity", *opacity_);
    if (rect_->is_visible() != shown) rect_->set_visible(shown);
}

std::string OverviewFadeView::report_json() const {
    std::ostringstream output;
    output << "{\"requests\": " << fade_.requests() << ", \"drawn_frames\": " << fade_.drawn_frames()
           << ", \"held_images\": " << held_images_ << ", \"capture\": \"" << capture_ << "\""
           << ", \"frames_per_fade\": " << presentation::ui::overview_fade_frames << "}";
    return output.str();
}

} // namespace eawr::presentation::godot_backend
