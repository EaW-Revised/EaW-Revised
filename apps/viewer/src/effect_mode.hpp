#pragma once

#include "presentation_constants.hpp"

#include <godot_cpp/classes/node3d.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace eawr::presentation::godot_backend {

// A self-contained viewer mode that spawns one particle effect, advances it a
// fixed number of frames at a fixed step with a fixed seed, captures, and
// writes a run report, including parent birth/death events when present.
// Like MapMode it owns its renderer, VFS mount and frame
// loop; it parses its own options so the shared host only hands over.
class EffectMode final {
public:
    struct Options final {
        std::filesystem::path game_root;
        std::filesystem::path mod_root;
        std::string profile;
        std::string profile_error;
        // Logical VFS path of the particle ALO, for example
        // "data/art/models/p_synthetic_effect.alo".
        std::string effect_path;
        // Optional "<model logical path>:<bone>" host; the effect follows the
        // named attachment. `animation_path` optionally animates the host.
        std::string attach;
        // Explicit EnhancedMesh host and named proxy. Both options are required
        // together, and cannot be combined with --eawr-effect-attach.
        std::string proxy_host;
        std::string proxy_name;
        std::string animation_path;
        std::uint32_t seed{20260922U};
        std::uint32_t frames{60};
        float delta_seconds{presentation_constants::logical_frame_seconds};
        std::uint32_t capacity{8192};
        std::filesystem::path report_path;
        std::filesystem::path capture_path;
    };

    // True when the command line asks for this mode (`--eawr-effect`).
    [[nodiscard]] static bool requested();
    // Reads this mode's options from the Godot user command line.
    [[nodiscard]] static Options from_command_line();

    explicit EffectMode(Options options);
    ~EffectMode();
    EffectMode(EffectMode&&) noexcept;
    EffectMode& operator=(EffectMode&&) noexcept;
    EffectMode(const EffectMode&) = delete;
    EffectMode& operator=(const EffectMode&) = delete;

    // Mounts, loads, plans and spawns. A false return has written the report.
    [[nodiscard]] bool ready(godot::Node3D& host);
    // One frame; returns the exit code once the report has been written.
    [[nodiscard]] std::optional<int> process();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
