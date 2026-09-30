#pragma once

// #848: the battle UI in the tactical overview levels x1 and x2
// (docs/behaviour/foc-battle-selection.md#the-battle-ui-in-the-overview-848, V-5a to V-5h). What the
// live battle draws while either level is on, and the cross-fade every level change starts. The
// model is presentation state only: nothing here reaches a command, the replay or a hash.

#include <cstdint>
#include <optional>

namespace eawr::presentation::ui {

// What the battle draws, for the overview on (x1 or x2) or off. V-5a: x1 and x2 hide the same set.
struct OverviewUi final {
    // V-5b: the tactical shell and everything placed on it (faceplate and help droid, the minimap
    // frame, the unit cards, the ability, order and options buttons, the time panel, the planet name).
    bool tactical_shell{true};
    // V-5b: the pause banner while paused; the win/lose message and the end panel stay (V-5e).
    bool pause_banner{true};
    // V-5c: the radar's contents (blips, fog, the camera outline) stop updating and drawing.
    bool radar_contents{true};
    // V-5d: the unit brackets: health and shield bars, the control-group number and the status icons.
    bool unit_brackets{true};
    // V-5e: squadron icons (frame and bar), hardpoint reticles, selection circles, the drag box and
    // the pointer do not read the overview state and always draw.
    bool world_markers{true};
    // V-5f: the scene's distance fog (not the fog of war).
    bool distance_fog{true};

    friend bool operator==(const OverviewUi&, const OverviewUi&) = default;
};

[[nodiscard]] constexpr OverviewUi overview_ui(const bool overview_on) noexcept {
    if (!overview_on) return {};
    return OverviewUi{.tactical_shell = false, .pause_banner = false, .radar_contents = false,
                      .unit_brackets = false, .world_markers = true, .distance_fog = false};
}

// V-5g: the fade's fixed steps, per drawn frame: opacity 1 - n x 0.025 / 0.25 on the n-th frame after
// the held image, 0.9 down to 0.1 over nine frames, gone on the tenth.
inline constexpr float overview_fade_length = 0.25F;
inline constexpr float overview_fade_step = 0.025F;
inline constexpr std::uint32_t overview_fade_frames = 9;

[[nodiscard]] constexpr float overview_fade_opacity(const std::uint32_t frame) noexcept {
    return 1.0F - static_cast<float>(frame) * overview_fade_step / overview_fade_length;
}

// V-5g: the cross-fade of the last old-level image over the next drawn frames. request() on a level
// change holds the image; each drawn frame then takes next_frame()'s opacity for it. A request
// during a running fade restarts it with a new held image (V-5g leaves that case unverified; the
// remake's choice is on the fidelity list).
class OverviewFade final {
public:
    void request() noexcept {
        frame_ = 0;
        running_ = true;
        ++requests_;
    }
    // The held image's opacity for the frame about to draw, or nullopt once the fade is over.
    [[nodiscard]] std::optional<float> next_frame() noexcept {
        if (!running_) return std::nullopt;
        ++frame_;
        if (frame_ > overview_fade_frames) {
            running_ = false;
            return std::nullopt;
        }
        ++drawn_;
        return overview_fade_opacity(frame_);
    }
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] std::uint64_t requests() const noexcept { return requests_; }
    // Frames drawn with the held image over them, over every fade so far.
    [[nodiscard]] std::uint64_t drawn_frames() const noexcept { return drawn_; }

private:
    std::uint32_t frame_{};
    bool running_{};
    std::uint64_t requests_{};
    std::uint64_t drawn_{};
};

} // namespace eawr::presentation::ui
