#include "eawr/sim/tactical/ion.hpp"

#include "../replay_internal.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::tactical {
namespace {

using math::Fixed;

constexpr std::int64_t one_raw = Fixed::scale;

[[nodiscard]] std::uint32_t clamp_frames(const std::int64_t frames) noexcept {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(frames, 0, std::numeric_limits<std::uint32_t>::max()));
}

} // namespace

bool valid_ion_stun(const IonStunShot& shot) noexcept {
    const auto fraction = [](const Fixed value) { return value.raw() >= 0 && value.raw() <= one_raw; };
    return shot.frames >= 1 && shot.frames <= max_ion_stun_frames && fraction(shot.speed_reduction)
        && fraction(shot.rate_reduction);
}

IonStunState ion_stun(const std::optional<IonStunState>& current, const IonStunShot& shot, const std::uint64_t frame) noexcept {
    IonStunState next;
    // IS-04: a stacking shot extends a stun that has not ended before this frame.
    if (shot.stack && current && current->end_frame >= frame) {
        next.end_frame = current->end_frame + shot.frames;
    } else {
        next.end_frame = frame + shot.frames;
    }
    next.speed_reduction = shot.speed_reduction;
    next.rate_reduction = shot.rate_reduction;
    return next;
}

bool ion_stunned(const std::optional<IonStunState>& state, const std::uint64_t frame) noexcept {
    return state && frame < state->end_frame;
}

Fixed ion_speed_factor(const std::optional<IonStunState>& state, const std::uint64_t frame) noexcept {
    if (!ion_stunned(state, frame)) return Fixed::from_raw(one_raw);
    return Fixed::from_raw(one_raw - state->speed_reduction.raw());
}

Fixed ion_fire_rate(const std::optional<IonStunState>& state, const std::uint64_t frame) noexcept {
    if (!ion_stunned(state, frame)) return Fixed::from_raw(one_raw);
    return Fixed::from_raw(one_raw - state->rate_reduction.raw());
}

std::uint32_t fire_rate_recharge(const std::uint32_t frames, const Fixed rate) noexcept {
    if (rate.raw() <= 0 || rate.raw() >= one_raw) return frames;
    // trunc(frames / rate + 0.5), exactly: (2 x frames x scale + rate) / (2 x rate).
    return clamp_frames((2 * static_cast<std::int64_t>(frames) * one_raw + rate.raw()) / (2 * rate.raw()));
}

std::uint32_t fire_rate_pulses(const std::uint32_t pulses, const Fixed rate) noexcept {
    if (rate.raw() == one_raw) return pulses;
    const auto factor = std::max<std::int64_t>(rate.raw(), 0);
    return clamp_frames((static_cast<std::int64_t>(pulses) * factor + one_raw / 2) / one_raw);
}

std::uint32_t fire_rate_gap(const std::uint32_t frames, const Fixed rate) noexcept {
    if (rate.raw() == one_raw) return frames;
    if (rate.raw() <= 0) return 0;
    return clamp_frames((2 * static_cast<std::int64_t>(frames) * one_raw + rate.raw()) / (2 * rate.raw()));
}

void append_ion_stun(std::vector<std::uint8_t>& bytes, const IonStunState& state) {
    sim::detail::append_u64(bytes, state.end_frame);
    sim::detail::append_i64(bytes, state.speed_reduction.raw());
    sim::detail::append_i64(bytes, state.rate_reduction.raw());
}

} // namespace eawr::sim::tactical
