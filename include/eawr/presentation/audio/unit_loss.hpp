#pragma once

#include <cstdint>

namespace eawr::presentation::audio {

// BA-65: notification-time losses, independent of selection, fog and death effects.
// Ordinary SFX admission supplies instance/overlap throttling; refused requests never retry.
template<class Losses, class Player, class Resolve, class Request>
void present_unit_losses(const Losses& losses, Player local, Resolve resolve, Request request) {
    for (const auto& loss : losses) {
        if (loss.owner != local) continue;
        const auto* cue = resolve(loss.type);
        if (!cue) continue;
        for (std::uint64_t count = 0; count < loss.count; ++count) request(cue);
    }
}

} // namespace eawr::presentation::audio
