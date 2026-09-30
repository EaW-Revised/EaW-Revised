#include "../src/viewer_path.hpp"

#include <iostream>
#include <string>

namespace {

bool expect(const bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

} // namespace

int main() {
    using eawr::presentation::godot_backend::ViewerPath;

    bool passed = true;
    const ViewerPath default_scene{std::string{"res://common/scene.json"}};
    passed &= expect(default_scene.is_godot_resource(), "res:// was not classified as a resource");
    passed &= expect(
        default_scene.value() == "res://common/scene.json",
        "res:// resource identifier was changed");

    const ViewerPath user_resource{std::string{"user://captures/frame.png"}};
    passed &= expect(
        user_resource.is_godot_resource(), "user:// was not classified as a resource");

    const ViewerPath malformed{std::string{"res:/common/scene.json"}};
    passed &= expect(
        !malformed.is_godot_resource(), "malformed resource identifier was rewritten or accepted");

    const std::u8string unicode_native_u8 = u8"fixtures/ユニコード/replay.eawr-replay";
    const std::string unicode_native(
        reinterpret_cast<const char*>(unicode_native_u8.data()), unicode_native_u8.size());
    const ViewerPath native{unicode_native};
    passed &= expect(!native.is_godot_resource(), "native path was classified as a resource");
    passed &= expect(
        native.native().generic_u8string() == std::u8string(
            reinterpret_cast<const char8_t*>(unicode_native.data()), unicode_native.size()),
        "native UTF-8 path did not round-trip");

    return passed ? 0 : 1;
}
