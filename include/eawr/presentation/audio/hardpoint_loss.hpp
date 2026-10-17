#pragma once

#include "eawr/presentation/audio/sfx.hpp"

#include <map>
#include <span>
#include <string>
#include <string_view>

namespace eawr::presentation::audio {

// BA-64: a blank derived class entry deliberately clears its inherited cue.
template<class Resolve>
void read_hardpoint_loss(std::string_view text, std::map<std::string, const SfxEvent*>& cues, Resolve resolve) {
    const auto pair = split_list(text);
    if (pair.empty() || pair.size() > 2) return;
    cues[upper(pair[0])] = pair.size() == 2 ? resolve(pair[1]) : nullptr;
}

// BA-64 / WCC-63: caller supplies notification-time standing hardpoints.
// The ordinary SFX admission path supplies overlap/voice throttling; no retry.
[[nodiscard]] inline const SfxEvent* hardpoint_loss_cue(bool local_owner, bool weapon,
    bool weapons_remaining, std::size_t lost, std::span<const std::uint32_t> standing,
    std::span<const std::string> types, const std::map<std::string, const SfxEvent*>& cues,
    const SfxEvent* all_weapons) {
    if (!local_owner || lost >= types.size()) return nullptr;
    if (weapon && !weapons_remaining && all_weapons) return all_weapons;
    for (const auto index : standing) {
        if (index < types.size() && types[index] == types[lost]) return nullptr;
    }
    const auto cue = cues.find(upper(types[lost]));
    return cue == cues.end() ? nullptr : cue->second;
}

} // namespace eawr::presentation::audio
