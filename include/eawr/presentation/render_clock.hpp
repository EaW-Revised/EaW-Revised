#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace eawr::presentation {

// FD-74: a tactical render clock observes committed logical frames, never wall
// time or the requested speed. Pause admits no frames; faster admission advances
// every scene consumer together. This clock is presentation-owned and read-only
// with respect to the session.
class RenderClock final {
public:
    void observe(const std::uint64_t admitted_frames) noexcept {
        admitted_frames_ = std::max(admitted_frames_, admitted_frames);
    }

    [[nodiscard]] std::uint64_t admitted_frames() const noexcept { return admitted_frames_; }
    [[nodiscard]] std::uint32_t sample() const noexcept {
        return static_cast<std::uint32_t>(std::min(admitted_frames_,
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
    }

private:
    std::uint64_t admitted_frames_{};
};

} // namespace eawr::presentation
