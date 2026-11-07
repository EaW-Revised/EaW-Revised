#pragma once

// Plain-words build labels for the window title and the performance overlay. Two separate facts:
// the viewer library (our code: release or debug) and the engine binary hosting it (editor,
// debug export template or release export template). The engine appends " (DEBUG)" to the
// window title whenever the engine binary has debug support; the viewer writes the full statement and
// keeps that " (DEBUG)" suffix, so a debug-enabled engine is never hidden. Pure text, no engine types, so it is
// unit tested.

#include <string>

namespace eawr::presentation::ui {

// Editor and debug-template engines are debug-enabled; only a release template is not.
[[nodiscard]] inline std::string engine_build_kind(const bool editor, const bool debug_build) {
    if (editor) return "editor, debug-enabled";
    return debug_build ? "debug template, debug-enabled" : "release template";
}

// "4.7.2 editor, debug-enabled"
[[nodiscard]] inline std::string engine_summary(const long long major, const long long minor, const long long patch,
                                                const std::string& kind) {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch) + " " + kind;
}

// "EAWR Viewer (viewer: release; engine: Godot 4.7.2 editor, debug-enabled, eawr fad06b579) (DEBUG)"
// `signature` is EngineSignature::display (engine_signature.hpp). A debug-enabled engine keeps the engine's own
// " (DEBUG)" suffix: setting the title through the DisplayServer drops it, so it is written back here.
[[nodiscard]] inline std::string window_title(const std::string& application_name, const std::string& viewer_build,
                                              const std::string& engine, const std::string& signature,
                                              const bool engine_debug_enabled) {
    return application_name + " (viewer: " + viewer_build + "; engine: Godot " + engine + ", " + signature + ")"
        + (engine_debug_enabled ? " (DEBUG)" : "");
}

// The two overlay rows.
[[nodiscard]] inline std::string overlay_viewer_row(const std::string& viewer_build) { return "viewer: " + viewer_build; }
[[nodiscard]] inline std::string overlay_engine_row(const std::string& engine) { return "engine: " + engine; }
[[nodiscard]] inline std::string overlay_signature_row(const std::string& signature) { return "engine: " + signature; }

} // namespace eawr::presentation::ui
