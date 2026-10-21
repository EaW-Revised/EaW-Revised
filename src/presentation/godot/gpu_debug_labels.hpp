#pragma once

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>

namespace eawr::presentation::godot_backend {

// Cache an identity without registering/taking over a ResourceCache entry.
// Shader forwards it to shader_create_from_code when its RID is first needed;
// requesting the RID before set_code would instead return an empty RID.
inline void name_canvas_shader(godot::Shader& shader, const char* identity) {
    shader.set_name(identity);
    shader.set_path_cache(identity);
}

// Only bracket commands we actually submit. RenderingServer owns the stock
// UI_PASS and BLIT_PASS; Control::_draw records CPU canvas commands, not RD draws.
class GpuDebugLabel final {
public:
    GpuDebugLabel(godot::RenderingServer& server, godot::RenderingDevice& device,
                  const char* name, const bool enabled) {
        if (enabled && server.is_on_render_thread()) {
            device_ = &device;
            device_->draw_command_begin_label(name, godot::Color(0.2F, 0.7F, 1.0F));
        }
    }
    ~GpuDebugLabel() { if (device_) device_->draw_command_end_label(); }
    GpuDebugLabel(const GpuDebugLabel&) = delete;
    GpuDebugLabel& operator=(const GpuDebugLabel&) = delete;
private:
    godot::RenderingDevice* device_{};
};

} // namespace eawr::presentation::godot_backend
