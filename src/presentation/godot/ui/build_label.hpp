#pragma once

// Runtime facts for the build labels (the text composition lives in eawr/presentation/ui/build_label.hpp).

#include "eawr/presentation/engine_signature.hpp"
#include "eawr/presentation/ui/build_label.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <string>

namespace eawr::presentation::godot_backend {

// Our library: "debug" only when this library itself was compiled without NDEBUG.
[[nodiscard]] constexpr const char* viewer_build_type() noexcept {
#ifdef NDEBUG
    return "release";
#else
    return "debug";
#endif
}

[[nodiscard]] inline std::string engine_build_type() {
    auto* os = godot::OS::get_singleton();
    if (os == nullptr) return "unknown";
    return presentation::ui::engine_build_kind(os->has_feature("editor"), os->is_debug_build());
}

[[nodiscard]] inline std::string engine_version_field(const char* key) {
    auto* engine = godot::Engine::get_singleton();
    if (engine == nullptr) return {};
    return std::string(godot::String(engine->get_version_info().get(key, "")).utf8().get_data());
}

[[nodiscard]] inline std::string engine_build_name() { return engine_version_field("build"); }

// "eawr <hash>" or "UNPATCHED ENGINE (...)": the startup engine guard decides (engine_guard_startup.hpp), the viewer displays.
[[nodiscard]] inline std::string engine_signature() { return presentation::current_engine_signature().display; }
[[nodiscard]] inline bool engine_is_patched() { return presentation::current_engine_signature().patched; }

// True for an editor or debug-template engine (the engine itself titles those windows "(DEBUG)").
[[nodiscard]] inline bool engine_debug_enabled() {
    auto* os = godot::OS::get_singleton();
    return os != nullptr && (os->has_feature("editor") || os->is_debug_build());
}

[[nodiscard]] inline std::string engine_version() { return engine_version_field("string"); }
[[nodiscard]] inline std::string engine_hash() { return engine_version_field("hash"); }

[[nodiscard]] inline std::string engine_summary() {
    auto* engine = godot::Engine::get_singleton();
    if (engine == nullptr) return engine_build_type();
    const godot::Dictionary info = engine->get_version_info();
    return presentation::ui::engine_summary(static_cast<int64_t>(info.get("major", 0)), static_cast<int64_t>(info.get("minor", 0)),
                                            static_cast<int64_t>(info.get("patch", 0)), engine_build_type());
}

} // namespace eawr::presentation::godot_backend
