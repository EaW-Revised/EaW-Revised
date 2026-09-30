#pragma once

#include <godot_cpp/classes/node3d.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace eawr::presentation::godot_backend {

// A self-contained viewer mode (`--eawr-unit <ALO>`) that draws every
// drawable mesh of one model, optionally under one ALA clip, at a list of
// clip times, and writes the frames as one strip PNG plus a run report. It
// exists to judge skinned and rigid animation by eye (P1-03). Like EffectMode
// it owns its renderer, VFS mount and frame loop and parses its own options.
class UnitMode final {
public:
    struct Options final {
        std::filesystem::path game_root;
        std::filesystem::path mod_root;
        std::string model_path;
        std::string animation_path;
        // Comma-separated clip times in seconds (looped); one strip tile each.
        std::string times{"0"};
        // Render-basis direction from the unit towards the eye.
        std::string view{"0.8,0.5,1.0"};
        std::optional<int> lod;
        // Optional "r,g,b" team colour for colorized effects.
        std::string colour;
        std::filesystem::path report_path;
        std::filesystem::path capture_path;
    };

    // True when the command line asks for this mode (`--eawr-unit`).
    [[nodiscard]] static bool requested();
    [[nodiscard]] static Options from_command_line();

    explicit UnitMode(Options options);
    ~UnitMode();
    UnitMode(UnitMode&&) noexcept;
    UnitMode& operator=(UnitMode&&) noexcept;
    UnitMode(const UnitMode&) = delete;
    UnitMode& operator=(const UnitMode&) = delete;

    // Mounts, loads, plans, uploads and frames. A false return has written the report.
    [[nodiscard]] bool ready(godot::Node3D& host);
    // One frame; returns the exit code once the report has been written.
    [[nodiscard]] std::optional<int> process();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
