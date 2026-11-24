#pragma once

// A tactical script's screen fade (Fade_Screen_Out / Fade_Screen_In / Fade_On / Fade_Off, docs/behaviour/tactical-story-cinematic.md,
// TSC-11). One full-screen black layer with an opacity: Fade_Screen_Out(t) clears it and raises it to opaque over t seconds,
// Fade_Screen_In(t) makes it opaque and lowers it to clear over t seconds, Fade_On is opaque at once, Fade_Off clear at once. The
// layer keeps its last state (a faded-out screen stays black until the script fades in or turns the fade off). Engine-free and
// header-only; the seconds are render-frame time, as the original's own fade timer is.

#include <algorithm>

namespace eawr::presentation::ui {

class ScreenFade {
public:
    // Clear, then to opaque over `seconds`. A non-finite or negative time is taken as 0 (at once).
    void fade_out(const double seconds) noexcept { begin(0.0F, seconds); }
    // Opaque, then to clear over `seconds`.
    void fade_in(const double seconds) noexcept { begin(1.0F, seconds); }
    void on() noexcept { set(1.0F); }
    void off() noexcept { set(0.0F); }
    // One frame of the fade in progress.
    void advance(const double delta) noexcept {
        if (!active_) return;
        remaining_ -= std::max(delta, 0.0);
        if (remaining_ > 0.0) {
            const float done = static_cast<float>(1.0 - remaining_ / total_);
            opacity_ = from_ + (to_ - from_) * std::clamp(done, 0.0F, 1.0F);
            return;
        }
        opacity_ = to_;
        active_ = false;
    }

    [[nodiscard]] float opacity() const noexcept { return opacity_; }
    [[nodiscard]] bool visible() const noexcept { return opacity_ > 0.0F; }
    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    void set(const float value) noexcept {
        opacity_ = value;
        active_ = false;
    }
    void begin(const float from, const double seconds) noexcept {
        from_ = from;
        to_ = 1.0F - from;
        opacity_ = from;
        total_ = seconds > 0.0 && seconds < 1.0e9 ? seconds : 0.0;
        remaining_ = total_;
        active_ = true;
        if (total_ <= 0.0) { opacity_ = to_; active_ = false; } // a zero time ends at once
    }

    float opacity_{};
    float from_{}, to_{};
    double total_{}, remaining_{};
    bool active_{};
};

} // namespace eawr::presentation::ui
