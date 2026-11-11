#include "viewer_host_internal.hpp"
#include "renderer_prewarm_arrays.hpp"
#include <godot_cpp/core/class_db.hpp>
#include "eawr/core/text.hpp"
#include "audio_output.hpp"
#include "startup_trace.hpp"
#include "viewer_mounted_mods.hpp"
#include "gpu_memory_trace.hpp"
#include <godot_cpp/classes/rendering_server.hpp>

namespace eawr::presentation::godot_backend {
namespace viewer_host_detail {

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), converted.length());
}

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) {
    return core::iequals(left, right);
}

[[nodiscard]] std::string hash_bytes(const std::span<const std::byte> bytes) {
    return sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

} // namespace viewer_host_detail

namespace {

// "<width>x<height>" in whole pixels, each 1..16384.






} // namespace

ViewerHost::ViewerHost() = default;
ViewerHost::~ViewerHost() {
    shutdown_mounted_mods();
    shutdown_trace::mark("viewer host freed (members follow)");
}

void ViewerHost::_bind_methods() {
    godot::ClassDB::bind_static_method("ViewerHost", godot::D_METHOD("prewarm_mesh_layout", "skinned"),
        &ViewerHost::prewarm_mesh_layout);
}

godot::Dictionary ViewerHost::prewarm_mesh_layout(const bool skinned) {
    godot::Dictionary result;
    result["arrays"] = prewarm_mesh_arrays(skinned);
    result["format"] = static_cast<int64_t>(prewarm_mesh_format());
    return result;
}

void ViewerHost::on_battle_frame_drawn() {
    if (startup_trace.pending()) gpu_memory_trace.sample("battle_first_frame");
    startup_trace.drawn();
}
void ViewerHost::on_setup_frame_drawn() {
    gpu_memory_trace.sample("setup_after_battle");
    startup_trace.setup_drawn();
}











void ViewerHost::stop(const int exit_code) {
    if (auto* input = Input::get_singleton()) input->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
    completed_ = true;
    const int result = front_end_ && !front_end_->close() ? 2 : exit_code;
    if (get_tree()) get_tree()->quit(result);
}

} // namespace eawr::presentation::godot_backend
