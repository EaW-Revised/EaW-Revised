#include "perf_context.hpp"

#include <sstream>

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "eawr/core/json_text.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/presentation/ui/render_thread_setting.hpp"
#include "eawr/presentation/ui/settings.hpp"
#include "particle_workers.hpp"
#include "settings_store.hpp"

namespace eawr::presentation::godot_backend {

using namespace godot;

namespace {

std::string text_of(const String& value) { return std::string(value.utf8().get_data()); }

const char* window_mode_name(const DisplayServer::WindowMode mode) {
    switch (mode) {
    case DisplayServer::WINDOW_MODE_WINDOWED: return "windowed";
    case DisplayServer::WINDOW_MODE_MINIMIZED: return "minimized";
    case DisplayServer::WINDOW_MODE_MAXIMIZED: return "maximized";
    case DisplayServer::WINDOW_MODE_FULLSCREEN: return "fullscreen";
    case DisplayServer::WINDOW_MODE_EXCLUSIVE_FULLSCREEN: return "exclusive-fullscreen";
    default: return "unknown";
    }
}

const char* vsync_name(const DisplayServer::VSyncMode mode) {
    switch (mode) {
    case DisplayServer::VSYNC_DISABLED: return "off";
    case DisplayServer::VSYNC_ENABLED: return "on";
    case DisplayServer::VSYNC_ADAPTIVE: return "adaptive";
    case DisplayServer::VSYNC_MAILBOX: return "mailbox";
    default: return "unknown";
    }
}

const char* gpu_type_name(const RenderingDevice::DeviceType type) {
    switch (type) {
    case RenderingDevice::DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case RenderingDevice::DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case RenderingDevice::DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case RenderingDevice::DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

void quoted(std::ostringstream& out, const char* key, const std::string& value) {
    out << '"' << key << "\":" << core::json_quote(value);
}

// One pool: the saved choice (0 = Automatic), the automatic count of this PC and what applies.
void pool(std::ostringstream& out, const char* key, const int setting, const std::size_t automatic) {
    out << '"' << key << "\":{\"setting\":" << setting << ",\"automatic\":" << automatic << ",\"mode\":\""
        << (setting > 0 ? "manual" : "automatic") << "\",\"resolved\":" << (setting > 0 ? static_cast<std::size_t>(setting) : automatic) << '}';
}

} // namespace

std::string perf_context_fields() {
    std::ostringstream out;
    auto* rendering = RenderingServer::get_singleton();
    auto* display = DisplayServer::get_singleton();
    auto* os = OS::get_singleton();
    if (rendering != nullptr) {
        quoted(out, "gpu", text_of(rendering->get_video_adapter_name()));
        out << ',';
        quoted(out, "gpu_vendor", text_of(rendering->get_video_adapter_vendor()));
        out << ",\"gpu_type\":\"" << gpu_type_name(rendering->get_video_adapter_type()) << "\",";
        quoted(out, "gpu_api", text_of(rendering->get_video_adapter_api_version()));
        out << ',';
        quoted(out, "renderer", text_of(rendering->get_current_rendering_method()));
        out << ',';
        quoted(out, "rd_driver", text_of(rendering->get_current_rendering_driver_name()));
        out << ',';
    }
    if (os != nullptr) {
        const PackedStringArray driver = os->get_video_adapter_driver_info();
        std::string joined;
        for (int64_t i = 0; i < driver.size(); ++i) {
            if (i != 0) joined += ' ';
            joined += text_of(driver[i]);
        }
        quoted(out, "gpu_driver", joined);
        out << ',';
    }
    if (display != nullptr) {
        const Vector2i size = display->window_get_size();
        out << "\"res\":[" << size.x << ',' << size.y << "],\"window\":\"" << window_mode_name(display->window_get_mode())
            << "\",\"vsync\":\"" << vsync_name(display->window_get_vsync_mode()) << "\",";
    }
    out << "\"fps_cap\":" << Engine::get_singleton()->get_max_fps();
    const bool separate = rendering != nullptr && !rendering->is_on_render_thread();
    out << ",\"render_thread\":\"" << (separate ? "separate" : "main") << '"';
    if (const auto* settings = ActiveSettings::get()) {
        const auto& video = settings->video;
        out << ",\"render_thread_setting\":\""
            << (video.render_thread == presentation::ui::RenderThreadSetting::separate ? "separate" : "safe") << "\",\"video\":{"
            << "\"windowed\":" << (video.windowed ? "true" : "false") << ",\"vsync\":" << (video.vsync ? "true" : "false")
            << ",\"fps_limit\":" << video.frame_rate_limit << ",\"detail_level\":" << video.detail_level
            << ",\"mip_drop\":" << video.texture_mip_drop << ",\"supersampling\":" << video.supersampling
            << ",\"render_profile\":" << static_cast<int>(video.render_profile) << ",\"depth_prepass\":"
            << (video.depth_prepass ? "true" : "false") << ",\"widescreen\":" << (video.widescreen ? "true" : "false")
            << ",\"detail\":[";
        for (std::size_t i = 0; i < video.detail.size(); ++i) out << (i != 0 ? "," : "") << video.detail[i];
        out << "]}";
        out << ",\"threads\":{\"hardware\":" << platform::ThreadWorkerAdapter::hardware_worker_count() << ',';
        pool(out, "sim", video.simulation_threads, platform::LiveSession::game_worker_count());
        out << ',';
        pool(out, "particles", video.particle_threads,
            ParticleWorkers::default_count(platform::ThreadWorkerAdapter::hardware_worker_count()));
        out << '}';
    } else {
        out << ",\"threads\":{\"hardware\":" << platform::ThreadWorkerAdapter::hardware_worker_count() << ',';
        pool(out, "sim", 0, platform::LiveSession::game_worker_count());
        out << ',';
        pool(out, "particles", 0, ParticleWorkers::default_count(platform::ThreadWorkerAdapter::hardware_worker_count()));
        out << '}';
    }
    return out.str();
}

} // namespace eawr::presentation::godot_backend
