#pragma once

// The battle camera's letterbox bands (rules BCAM-04 and HCB-11c): two black
// bands, each `Camera_FX_Manager_Letterbox_Height` of the screen height, slide
// in while the cinematic camera runs and out again when it ends. Engine-free
// and header-only: one progress value in 0..1 and the band fraction it gives.

#include <algorithm>

namespace eawr::presentation::ui {

// BCAM-04 (debug build): the bands come in over half a second and leave over
// half a second. Retail turns them off at once when the game is not playing;
// callers ask for that by resetting the progress to zero themselves.
inline constexpr double movie_letterbox_slide_seconds = 0.5;

// One step of the slide: toward 1 while the mode is on, toward 0 while it is off.
[[nodiscard]] constexpr float advance_movie_letterbox(const float progress, const bool on, const double delta,
                                                      const double slide_seconds = movie_letterbox_slide_seconds) {
    const double step = slide_seconds > 0.0 ? std::max(delta, 0.0) / slide_seconds : 1.0;
    const float moved = progress + static_cast<float>(on ? step : -step);
    return std::clamp(moved, 0.0F, 1.0F);
}

// Fraction of the screen height one band covers: the authored full height scaled by the slide.
[[nodiscard]] constexpr float movie_letterbox_band_fraction(const float authored_height, const float progress) {
    return authored_height * std::clamp(progress, 0.0F, 1.0F);
}

// The battle HUD's one set of bands, asked for by two owners (legacy #6634, HCB-75): the cinematic battle camera (slides over
// half a second, BCAM-04) and a tactical script's Letter_Box_In / Letter_Box_Out (slides over the seconds the script gave, 0 = at
// once). The bands and the hidden game GUI are on while EITHER asks for them, so a script that lowers the letterbox during a
// battle-camera shot leaves it up until the shot ends, and the end of a shot leaves a script's letterbox up. The owner whose
// request changes whether the bands are wanted sets the slide time. Pure state, no engine types.
class CinematicBands {
public:
    // The battle camera. `immediate` (the game is not playing) drops the bands at once, unless a script still wants them.
    void set_movie(const bool on, const bool immediate = false) noexcept {
        const bool before = wanted();
        movie_ = on;
        if (before != wanted()) slide_seconds_ = movie_letterbox_slide_seconds;
        if (!wanted() && immediate) progress_ = 0.0F;
    }
    // A script's request: on (In) or off (Out) over `seconds` (finite, not negative; anything else is ignored).
    void request_script(const bool on, const double seconds) noexcept {
        if (!(seconds >= 0.0) || seconds > 1.0e9) return;
        const bool before = wanted();
        script_ = on;
        if (before != wanted()) slide_seconds_ = seconds;
        if (seconds == 0.0) progress_ = wanted() ? 1.0F : 0.0F; // at once
    }
    // One frame: the slide toward the wanted state.
    void advance(const double delta) noexcept { progress_ = advance_movie_letterbox(progress_, wanted(), delta, slide_seconds_); }

    [[nodiscard]] bool wanted() const noexcept { return movie_ || script_; } // bands asked for: the game GUI is hidden
    [[nodiscard]] bool movie() const noexcept { return movie_; }
    [[nodiscard]] bool script() const noexcept { return script_; }
    [[nodiscard]] float progress() const noexcept { return progress_; }
    [[nodiscard]] bool visible() const noexcept { return wanted() || progress_ > 0.0F; }
    [[nodiscard]] double slide_seconds() const noexcept { return slide_seconds_; }

private:
    bool movie_{};
    bool script_{};
    float progress_{};
    double slide_seconds_{movie_letterbox_slide_seconds};
};

} // namespace eawr::presentation::ui
