#pragma once

#include <cstdint>
#include <array>
#include <cstddef>
#include <utility>

namespace eawr::presentation::audio {

enum class CommandCue : std::uint8_t { none, attack, attack_move, guard, move, stop, negative, count };

// BA-27: selecting a mode sounds only when it becomes armed. Stop sounds on the press.
[[nodiscard]] constexpr bool command_cue_on_press(CommandCue cue, bool armed) noexcept {
    return cue == CommandCue::stop || (armed && cue != CommandCue::none
        && cue != CommandCue::negative && cue != CommandCue::count);
}

// SND-24: containment selects the branch even if its authored line is absent.
template<class Cue>
[[nodiscard]] constexpr const Cue* move_response(bool asteroid, bool nebula, bool group,
    const Cue* asteroid_line, const Cue* nebula_line, const Cue* group_line, const Cue* ordinary) noexcept {
    const Cue* line = asteroid ? asteroid_line : nebula ? nebula_line : nullptr;
    return line ? line : group && group_line ? group_line : ordinary;
}

[[nodiscard]] constexpr bool assist_eligible(bool same_object, bool same_type, bool vehicle_thief) noexcept {
    return !same_object && !same_type && !vehicle_thief;
}

// SND-09/22: fixed voice slots retain a runtime chain. Only natural completion releases it;
// stealing, source removal, detachment and teardown cancel it. No frame-time storage growth.
template<class Cue, std::size_t Slots>
class ResponseChains final {
public:
    struct Entry { const Cue* cue{}; bool attack{}; std::uint64_t source{}; };
    void set(std::size_t voice, const Cue* cue, bool attack, std::uint64_t source) noexcept {
        if (voice < Slots) entries_[voice] = {cue, attack, source};
    }
    template<class IsLive>
    void cancel_missing(IsLive is_live) noexcept {
        for (auto& entry : entries_) {
            if (entry.cue && !is_live(entry.source)) entry = {};
        }
    }
    [[nodiscard]] Entry complete(std::size_t voice) noexcept {
        return voice < Slots ? std::exchange(entries_[voice], {}) : Entry{};
    }
    void cancel(std::size_t voice) noexcept { if (voice < Slots) entries_[voice] = {}; }
private:
    std::array<Entry, Slots> entries_{};
};

} // namespace eawr::presentation::audio
