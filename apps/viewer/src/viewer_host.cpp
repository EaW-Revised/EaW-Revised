#include "viewer_host_internal.hpp"
#include "audio_output.hpp"
#include "startup_trace.hpp"
#include <godot_cpp/classes/rendering_server.hpp>

namespace eawr::presentation::godot_backend {
namespace viewer_host_detail {

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), converted.length());
}

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](const char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
        };
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
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
ViewerHost::~ViewerHost() { shutdown_trace::mark("viewer host freed (members follow)"); }

void ViewerHost::_bind_methods() {}

void ViewerHost::on_battle_frame_drawn() { startup_trace.drawn(); }
void ViewerHost::on_setup_frame_drawn() { startup_trace.setup_drawn(); }











void ViewerHost::stop(const int exit_code) {
    completed_ = true;
    if (get_tree()) get_tree()->quit(exit_code);
}

} // namespace eawr::presentation::godot_backend
