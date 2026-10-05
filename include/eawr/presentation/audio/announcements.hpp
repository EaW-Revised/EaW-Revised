#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace eawr::presentation::audio {

// SND-42/64: consume eligibility before playback admission. A requested type
// cue suppresses first-enemy handling for this service call only.
class SightingAnnouncements final {
public:
    enum class Cue { none, type, enemy };
    [[nodiscard]] Cue observe(bool& type_seen, bool announce_type, bool busy_2d,
                              bool authored_type_cue, bool eligible_enemy) noexcept {
        if (announce_type && !type_seen) {
            type_seen = true;
            if (!busy_2d && authored_type_cue) return Cue::type;
        }
        if (eligible_enemy && !enemy_seen_) {
            enemy_seen_ = true;
            return Cue::enemy;
        }
        return Cue::none;
    }
    [[nodiscard]] bool enemy_seen() const noexcept { return enemy_seen_; }
private:
    bool enemy_seen_{};
};

// SND-44: FIFO, including duplicates; only the front is serviced. Storage is
// reserved here rather than allocated on a presentation frame. Full admission
// is a reported viewer safety limit, not a sourced speech cooldown.
template<class Event, std::size_t Capacity = 256>
class SpeechQueue final {
public:
    static_assert(Capacity > 0);
    struct Entry { const Event* event{}; std::string_view reason; };
    [[nodiscard]] bool push(const Event* event, std::string_view reason) noexcept {
        if (event == nullptr || size_ == Capacity) return false;
        entries_[(begin_ + size_) % Capacity] = {event, reason};
        ++size_;
        return true;
    }
    [[nodiscard]] const Entry* front() const noexcept { return size_ ? &entries_[begin_] : nullptr; }
    void pop() noexcept {
        if (size_ == 0) return;
        begin_ = (begin_ + 1) % Capacity;
        --size_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
private:
    std::array<Entry, Capacity> entries_{};
    std::size_t begin_{};
    std::size_t size_{};
};

// SND-48: one independent MP3 stream. Reserve before opening the file, so an
// equal/more important replacement that fails to open still retires the old
// handle. A refused lower-priority request leaves it intact.
class SpeechStream final {
public:
    using Handle = std::uint64_t;
    [[nodiscard]] bool admit(unsigned priority) noexcept {
        priority = std::clamp(priority, 1U, 5U);
        if (active_ && priority > priority_) return false;
        active_ = false;
        priority_ = priority;
        ++handle_;
        return true;
    }
    void opened() noexcept { active_ = true; }
    void finished(bool paused) noexcept { if (!paused) active_ = false; }
    [[nodiscard]] bool contains(Handle handle) const noexcept { return active_ && handle == handle_; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] Handle handle() const noexcept { return handle_; }
private:
    Handle handle_{};
    unsigned priority_{3};
    bool active_{};
};

// SND-61: an absolute ceiling before category/master gain. WAV voice responses
// and pending events do not activate it; a retained paused MP3 stream does.
[[nodiscard]] inline double speech_sfx_gain(double gain, bool stream_active, bool gui_dialog) noexcept {
    return stream_active && !gui_dialog ? std::min(gain, 0.3) : gain;
}

} // namespace eawr::presentation::audio
