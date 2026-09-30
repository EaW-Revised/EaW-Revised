#pragma once

#include "eawr/presentation/ui/overview_ui.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace eawr::presentation::godot_backend {

// #848 V-5g (docs/behaviour/foc-battle-selection.md): the cross-fade of every tactical overview level
// change. request() keeps the image the viewport last drew (world, HUD and all, still the old level)
// as a GPU-side copy; each later frame draws it over the whole view at the fixed step's opacity (0.9
// down to 0.1 over nine drawn frames). The copy stays on the GPU: no readback, so a level change does
// not stall the frame. Without a RenderingDevice backend (the Compatibility fallback) no image is kept
// and the frames are only counted.
class OverviewFadeView final {
public:
    explicit OverviewFadeView(godot::Node& host);
    ~OverviewFadeView();
    OverviewFadeView(const OverviewFadeView&) = delete;
    OverviewFadeView& operator=(const OverviewFadeView&) = delete;

    // A level change, before the new level's frame draws: hold `viewport`'s last drawn image.
    void request(godot::Viewport& viewport);
    // Once per frame, before it draws: the held image's opacity for it, or hidden.
    void frame();
    [[nodiscard]] std::optional<float> opacity() const noexcept { return opacity_; }
    [[nodiscard]] std::uint64_t requests() const noexcept { return fade_.requests(); }
    // The report's "overview_fade" object.
    [[nodiscard]] std::string report_json() const;

private:
    [[nodiscard]] bool hold(godot::Viewport& viewport);
    void release();

    presentation::ui::OverviewFade fade_;
    godot::CanvasLayer* layer_{};
    godot::TextureRect* rect_{};
    godot::Ref<godot::ShaderMaterial> material_;
    godot::Ref<godot::Texture2DRD> texture_;
    godot::RID held_;
    std::int64_t held_format_{-1};
    std::int32_t held_width_{};
    std::int32_t held_height_{};
    std::optional<float> opacity_;
    std::uint64_t held_images_{};
    std::string capture_{"none"};
};

} // namespace eawr::presentation::godot_backend
