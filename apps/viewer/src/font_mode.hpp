#pragma once

#include <godot_cpp/classes/node3d.hpp>

#include <filesystem>
#include <memory>
#include <optional>

namespace eawr::presentation::godot_backend {

// A self-contained viewer mode (`--eawr-fonts`) that loads the EmpireAtWar
// faces from the font cache (tools/fonts/extract_eaw_fonts.py, UI-05 #191),
// draws each one and a set of UI-F3 fallback requests on the 2D canvas, and
// writes one capture plus a report of what loaded and what each request
// resolved to. The cache is `--eawr-font-cache <dir>`, else EAWR_FONT_CACHE,
// else `out/fonts` of the checkout the project lives in.
class FontMode final {
public:
    struct Options final {
        std::filesystem::path font_cache;
        std::filesystem::path report_path;
        std::filesystem::path capture_path;
    };

    // True when the command line asks for this mode (`--eawr-fonts`).
    [[nodiscard]] static bool requested();
    [[nodiscard]] static Options from_command_line();

    explicit FontMode(Options options);
    ~FontMode();
    FontMode(FontMode&&) noexcept;
    FontMode& operator=(FontMode&&) noexcept;
    FontMode(const FontMode&) = delete;
    FontMode& operator=(const FontMode&) = delete;

    // Loads the cache and builds the sample canvas. A false return has written the report.
    [[nodiscard]] bool ready(godot::Node3D& host);
    // One frame; returns the exit code once the report has been written.
    [[nodiscard]] std::optional<int> process();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
