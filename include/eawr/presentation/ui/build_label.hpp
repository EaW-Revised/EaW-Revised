#pragma once

// Plain-words build labels for the window title and the performance overlay. Two separate facts:
// the viewer library (our code: release or debug) and the engine binary hosting it (editor,
// debug export template or release export template). The engine appends " (DEBUG)" to a window
// title it writes itself whenever the engine binary has debug support. The player-facing title never
// shows that: the viewer owns the final title (OwnedTitle) and writes it through the DisplayServer, which
// does not add the suffix; the build type stays visible in the performance overlay. Pure text, no engine
// types, so it is unit tested.

#include <string>
#include <utility>

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

// "EAWR Viewer (viewer: release; engine: Godot 4.7.2 editor, debug-enabled, eawr 5b733a90f)"
// With a launcher profile: "EAWR Viewer - <profile> (viewer: ...)". No profile (empty) = no " - " part.
// `signature` is EngineSignature::display (engine_signature.hpp). Never carries a "(DEBUG)" suffix.
[[nodiscard]] inline std::string window_title(const std::string& application_name, const std::string& profile_name,
                                              const std::string& viewer_build, const std::string& engine,
                                              const std::string& signature) {
    return application_name + (profile_name.empty() ? "" : " - " + profile_name)
        + " (viewer: " + viewer_build + "; engine: Godot " + engine + ", " + signature + ")";
}

// Holds the title the viewer wants and writes it through `sink` (DisplayServer::window_set_title). The engine
// rewrites the title (with its own "(DEBUG)") when a translation/title update runs, so the owner re-applies it
// after every such update (the first one, right after the host's _ready, reaches only the root window: the host
// re-applies from a deferred call). An empty title is never written.
class OwnedTitle {
public:
    template <class Sink> void set(std::string text, Sink&& sink) {
        text_ = std::move(text);
        reapply(sink);
    }
    template <class Sink> void reapply(Sink&& sink) const {
        if (!text_.empty()) sink(text_);
    }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }

private:
    std::string text_;
};

// The two overlay rows.
[[nodiscard]] inline std::string overlay_viewer_row(const std::string& viewer_build) { return "viewer: " + viewer_build; }
[[nodiscard]] inline std::string overlay_engine_row(const std::string& engine) { return "engine: " + engine; }
[[nodiscard]] inline std::string overlay_signature_row(const std::string& signature) { return "engine: " + signature; }

} // namespace eawr::presentation::ui
