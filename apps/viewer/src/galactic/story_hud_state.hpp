#pragma once

#include "eawr/core/from_chars.hpp"

#include "eawr/presentation/ui/movie_letterbox.hpp"
#include "eawr/sim/galactic/story_goals.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::viewer::galactic {

// The local HUD state a story plot drives (#236, G9/G10; WST-49, WST-75): which components flash, whether the human's
// controls are locked and whether map scrolling is locked. It only receives the presentation requests the session
// published for this seat (FLASH_GUI, LOCK_CONTROLS, SCROLL_LOCK) and never reaches the simulation: nothing here is
// saved, hashed or replayed, and no state of this class can change a sim result. Nothing knows a component's name; the
// HUD resolves the names it is given against the controls the loaded data authored.
class StoryHudState final {
public:
    // WST-75: a FLASH_GUI reward requests a component flash with arguments enabled, 0.5 and 3. The meaning of the two
    // numbers is unread in the debug build (WST-U09); the least visible reading is used: a 0.5 s pulse, three times.
    static constexpr double flash_period_seconds = 0.5;
    static constexpr int flash_pulses = 3;
    // A safety cap, not a rule of the original: a plot cannot grow the list without bound.
    static constexpr std::size_t max_flashes = 64;

    struct Flash {
        std::string component;
        double elapsed{};
    };

    // Applies one request published for this seat. Returns false for a request that is not a story HUD one.
    bool apply(const sim::galactic::story::ConsumerRequest& request) {
        using sim::galactic::story::Consumer;
        if (request.consumer == Consumer::gui && request.reward_name == "FLASH_GUI" && !request.parameters.empty()) {
            const auto& component = request.parameters.front();
            const auto same = std::find_if(flashes_.begin(), flashes_.end(), [&](const Flash& flash) { return equal(flash.component, component); });
            if (same != flashes_.end()) same->elapsed = 0.0; // a new request restarts the pulse
            else if (flashes_.size() < max_flashes) flashes_.push_back({component, 0.0});
            return true;
        }
        // legacy #6634: Letter_Box_In(seconds) / Letter_Box_Out(seconds) from a story script: the bands slide in or out over
        // the given seconds (0 = at once). The seconds arrive as text the host checked (finite, not negative); a request
        // that is not that is ignored here, never an abort.
        if (request.consumer == Consumer::camera && !request.parameters.empty()
            && (request.reward_name == "LETTER_BOX_IN" || request.reward_name == "LETTER_BOX_OUT")) {
            const auto& text = request.parameters.front();
            double seconds{};
            const auto parsed = eawr::core::from_chars(text.data(), text.data() + text.size(), seconds);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(seconds) || seconds < 0.0) return false;
            letterbox_on_ = request.reward_name == "LETTER_BOX_IN";
            letterbox_seconds_ = seconds;
            if (seconds == 0.0) letterbox_progress_ = letterbox_on_ ? 1.0F : 0.0F; // at once, before the next frame
            return true;
        }
        if (request.consumer == Consumer::control && !request.parameters.empty()) {
            const bool on = request.parameters.front() != "0";
            if (request.reward_name == "LOCK_CONTROLS") { controls_locked_ = on; return true; }
            if (request.reward_name == "SCROLL_LOCK") { scroll_locked_ = on; return true; }
        }
        return false;
    }

    // Moves every pulse on. A finished flash leaves the list and its component name goes to take_finished, so the HUD can
    // put the control back to its resting look.
    void advance(double seconds) {
        letterbox_progress_ = eawr::presentation::ui::advance_movie_letterbox(letterbox_progress_, letterbox_on(), seconds, letterbox_seconds_);
        const double length = flash_period_seconds * flash_pulses;
        for (auto& flash : flashes_) flash.elapsed += std::max(0.0, seconds);
        for (auto it = flashes_.begin(); it != flashes_.end();) {
            if (it->elapsed >= length) { finished_.push_back(std::move(it->component)); it = flashes_.erase(it); }
            else ++it;
        }
    }

    // 0 at rest, 1 at the peak of a pulse; the HUD maps it onto the control's brightness.
    [[nodiscard]] double intensity(const Flash& flash) const noexcept {
        constexpr double two_pi = 6.283185307179586;
        return 0.5 - 0.5 * std::cos(two_pi * flash.elapsed / flash_period_seconds);
    }
    [[nodiscard]] const std::vector<Flash>& flashes() const noexcept { return flashes_; }
    [[nodiscard]] std::vector<std::string> take_finished() { return std::exchange(finished_, {}); }

    [[nodiscard]] bool controls_locked() const noexcept { return controls_locked_; }
    [[nodiscard]] bool scroll_locked() const noexcept { return scroll_locked_; }
    // The letterbox a story script asked for: whether it is on (the target) and how far the bands have slid (0..1).
    // While it is on the game GUI is hidden (HCB-11c); the HUD owns the drawing and the authored band height.
    [[nodiscard]] bool letterbox_on() const noexcept { return letterbox_on_ || tour_on_; }
    // #6291 (GCT-07): the player's cinematic tour asks for the same bands, sliding over half a second, and for the GUI to be
    // hidden; a story script's own request is untouched and keeps them on after the tour ends.
    void set_tour(bool on) noexcept {
        if (tour_on_ == on) return;
        tour_on_ = on;
        if (!letterbox_on_) letterbox_seconds_ = tour_slide_seconds;
    }
    [[nodiscard]] bool tour_on() const noexcept { return tour_on_; }
    static constexpr double tour_slide_seconds = eawr::presentation::ui::movie_letterbox_slide_seconds;
    [[nodiscard]] float letterbox_progress() const noexcept { return letterbox_progress_; }
    [[nodiscard]] bool letterbox_visible() const noexcept { return letterbox_on() || letterbox_progress_ > 0.0F; }

private:
    [[nodiscard]] static bool equal(std::string_view a, std::string_view b) noexcept {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
    }

    std::vector<Flash> flashes_;
    std::vector<std::string> finished_;
    bool controls_locked_{}, scroll_locked_{};
    bool letterbox_on_{};
    bool tour_on_{};
    double letterbox_seconds_{};
    float letterbox_progress_{};
};

} // namespace eawr::viewer::galactic
