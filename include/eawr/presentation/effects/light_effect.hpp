#pragma once

// The per-object light effect (WSD-02, WSD-08; debug build): a timed colour
// the object's light scale is blended toward. Presentation only: it reads the
// frozen ability snapshot (remaining and total frames) and a presented frame
// count, never sim state.

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace eawr::presentation::light_effect {

enum class Kind : std::uint8_t { none, linear_pulse, color_flash };

struct State final {
    Kind kind{Kind::none};
    std::array<float, 3> color{};
    double start_frame{};     // presented logical frame the effect started at
    double duration_frames{}; // 0 never runs
    std::uint32_t pulses{1};

    friend constexpr bool operator==(const State&, const State&) noexcept = default;
};

// WSD-02: the countdown's light is a linear pulse in red (3, 0, 0), one pulse
// over twice the expiration; `elapsed_frames` of the countdown have passed.
[[nodiscard]] constexpr State self_destruct_pulse(const double now_frame, const std::uint32_t elapsed_frames,
                                                  const std::uint32_t expiration_frames) noexcept {
    return State{Kind::linear_pulse, {3.0F, 0.0F, 0.0F}, now_frame - static_cast<double>(elapsed_frames),
                 2.0 * static_cast<double>(expiration_frames), 1U};
}

// WSD-08: a cancel changes the light to a colour flash of the same colour for
// the countdown's remaining share (expiration x (1 - completed)).
[[nodiscard]] constexpr State self_destruct_cancel(const State& running, const double now_frame,
                                                   const std::uint32_t remaining_frames) noexcept {
    return State{Kind::color_flash, running.color, now_frame, static_cast<double>(remaining_frames), 1U};
}

// The light the effect gives `base` at `frame`; nullopt once it has ended or
// before it starts (the object keeps `base`). A linear pulse rises to its
// colour at half of each pulse and falls back; a colour flash starts at its
// colour and returns linearly to `base`.
[[nodiscard]] constexpr std::optional<std::array<float, 3>> apply(const State& state, const double frame,
                                                                  const std::array<float, 3>& base) noexcept {
    if (state.kind == Kind::none || state.duration_frames <= 0.0) return std::nullopt;
    const double elapsed = frame - state.start_frame;
    if (elapsed < 0.0 || elapsed >= state.duration_frames) return std::nullopt;
    std::array<float, 3> result = base;
    if (state.kind == Kind::linear_pulse) {
        const double pulse_length = state.duration_frames / static_cast<double>(std::max<std::uint32_t>(state.pulses, 1U));
        const double within = (elapsed - pulse_length * static_cast<double>(static_cast<std::uint64_t>(elapsed / pulse_length)))
            / pulse_length;
        const double scale = (within >= 0.5 ? 1.0 - within : within) * 2.0;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            result[channel] = static_cast<float>(base[channel] + (state.color[channel] - base[channel]) * scale);
        }
    } else {
        const double t = elapsed / state.duration_frames;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            result[channel] = static_cast<float>(state.color[channel] + (base[channel] - state.color[channel]) * t);
        }
    }
    return result;
}

} // namespace eawr::presentation::light_effect
