#include "battle_cursor.hpp"
#include "ui/theme_builder.hpp"
#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/ui/cursors.hpp"
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <algorithm>
#include <cmath>

namespace eawr::presentation::godot_backend {
using namespace godot;
BattleCursor::~BattleCursor() {
    if (capture_layer_ != nullptr) memdelete(capture_layer_);
    if (installed_) {
        if (auto* input = Input::get_singleton()) input->set_custom_mouse_cursor(Ref<Resource>{}, Input::CURSOR_ARROW);
    }
}
void BattleCursor::prepare(const vfs::Vfs& filesystem) {
    auto loaded = data::ui::load_cursors(filesystem);
    if (!loaded) { problems_.push_back(core::format_diagnostic(loaded.error())); return; }
    for (auto& [name, definition] : loaded.value()) {
        Pointer pointer;
        pointer.definition = std::move(definition);
        for (const auto& path : pointer.definition.frames) {
            const auto texture = assets::load_texture(filesystem, path);
            std::string failure;
            Ref<Image> image;
            if (texture) image = texture_image(texture.value(), failure);
            else failure = core::format_diagnostic(texture.error());
            if (image.is_null() || image->get_width() > 256 || image->get_height() > 256
                || pointer.definition.hot_x >= static_cast<std::uint32_t>(image->get_width())
                || pointer.definition.hot_y >= static_cast<std::uint32_t>(image->get_height())) {
                problems_.push_back(name + ": unusable frame " + path + " " + failure);
                break;
            }
            pointer.images.push_back(image);
            pointer.textures.push_back(ImageTexture::create_from_image(image));
        }
        if (pointer.images.empty()) problems_.push_back(name + ": no usable frames");
        pointers_.emplace(name, std::move(pointer));
    }
}
void BattleCursor::update(const std::string_view id, const double delta) {
    const bool changed = id_ != id;
    if (changed) { id_ = id; elapsed_ = 0.0; }
    else if (std::isfinite(delta)) elapsed_ += std::max(0.0, delta);
    auto found = pointers_.find(id_);
    if (found == pointers_.end() || found->second.images.empty()) found = pointers_.find("POINTER_NORMAL");
    if (found == pointers_.end() || found->second.images.empty()) {
        active_ = nullptr;
        frame_ = 0;
        if (installed_) {
            if (auto* input = Input::get_singleton()) {
                input->set_custom_mouse_cursor(Ref<Resource>{}, Input::CURSOR_ARROW);
                installed_ = false;
                ++system_restores_;
            }
        }
        if (capture_pointer_ != nullptr) capture_pointer_->hide();
        return;
    }
    const auto& pointer = found->second;
    active_ = &pointer;
    const auto next = ui::cursor_frame(elapsed_, pointer.definition.frame_delay, pointer.images.size());
    if (changed || !installed_ || next != frame_) {
        frame_ = next;
        if (auto* input = Input::get_singleton()) {
            input->set_custom_mouse_cursor(pointer.textures[frame_], Input::CURSOR_ARROW,
                Vector2(static_cast<real_t>(pointer.definition.hot_x), static_cast<real_t>(pointer.definition.hot_y)));
            installed_ = true;
        }
    }
}
void BattleCursor::write_report(std::ostream& output) const {
    output << "{\"id\": \"" << id_ << "\", \"frame\": " << frame_
        << ", \"hardware\": " << (installed_ ? "true" : "false") << ", \"definitions\": " << pointers_.size();
    output << ", \"overlay_visible\": " << (capture_pointer_ != nullptr && capture_pointer_->is_visible() ? "true" : "false");
    output << ", \"system_restores\": " << system_restores_;
    if (active_ != nullptr) {
        const auto& pointer = *active_;
        output << ", \"art_id\": \"" << pointer.definition.name << '\"';
        output << ", \"frames\": " << pointer.images.size() << ", \"hotspot\": [" << pointer.definition.hot_x
            << ',' << pointer.definition.hot_y << "], \"delay\": " << pointer.definition.frame_delay;
    }
    output << ", \"problems\": " << problems_.size() << '}';
}
void BattleCursor::composite(const Ref<Image> image, const Vector2 point) const {
    if (image.is_null() || active_ == nullptr) return;
    const auto& pointer = *active_;
    const auto source = pointer.images[std::min(frame_, pointer.images.size() - 1)];
    image->blend_rect(source, Rect2i(0, 0, source->get_width(), source->get_height()),
        Vector2i(static_cast<int>(std::lround(point.x)) - static_cast<int>(pointer.definition.hot_x),
                 static_cast<int>(std::lround(point.y)) - static_cast<int>(pointer.definition.hot_y)));
}
void BattleCursor::capture_overlay(Node& host, const Vector2 point, const bool enabled) {
    if (!enabled || active_ == nullptr) {
        if (capture_pointer_ != nullptr) capture_pointer_->hide();
        return;
    }
    if (capture_layer_ == nullptr) {
        // CU-03: viewport screenshots exclude hardware cursors, so diagnostic runs draw
        // the exact same frame above the HUD. This control never receives input.
        capture_layer_ = memnew(CanvasLayer);
        capture_layer_->set_layer(100);
        host.add_child(capture_layer_);
        capture_pointer_ = memnew(TextureRect);
        capture_pointer_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        capture_layer_->add_child(capture_pointer_);
    }
    const auto& pointer = *active_;
    capture_pointer_->show();
    capture_pointer_->set_texture(pointer.textures[frame_]);
    capture_pointer_->set_position(point - Vector2(static_cast<real_t>(pointer.definition.hot_x),
        static_cast<real_t>(pointer.definition.hot_y)));
    capture_pointer_->set_size(Vector2(static_cast<real_t>(pointer.images[frame_]->get_width()),
        static_cast<real_t>(pointer.images[frame_]->get_height())));
}
}
