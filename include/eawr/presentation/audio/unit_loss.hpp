#pragma once

#include <cstdint>
#include <optional>

namespace eawr::presentation::audio {

// BA-65: notification-time losses, independent of selection, fog and death effects.
// Ordinary SFX admission supplies instance/overlap throttling; refused requests never retry.
template<class Losses, class Player, class Types, class Play>
void present_unit_losses(const Losses& losses, Player local, const Types& types, Play play) {
    for (const auto& loss : losses) {
        if (loss.owner != local) continue;
        const auto type = types.find(loss.type);
        if (type == types.end()) continue;
        const auto* cue = type->second.unit_lost;
        if (!cue) continue;
        for (std::uint64_t count = 0; count < loss.count; ++count)
            play(cue, std::nullopt, false, "unit_lost");
    }
}

} // namespace eawr::presentation::audio
