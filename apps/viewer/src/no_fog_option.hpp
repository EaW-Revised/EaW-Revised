#pragma once

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

namespace eawr::viewer {

// --eawr-no-fog (the launcher's "Fog of war: off" launch option): an owner convenience, NOT a retail rule.
// It is the one flag behind both existing draw-only reveal switches: `--eawr-live-reveal on` for every
// tactical session (space and land) and `--eawr-galactic-reveal on` for the galaxy. The reveal is a read view
// over the truth the simulation publishes, so the simulation, its AI, saves, digests and replays never see
// it: a game or replay made with the flag is byte-identical to one made without.
//
// Single-player only. A multiplayer seat that sees what the other humans hide is a map hack, so a LAN
// battle and a multi-human or LAN campaign refuse the flag with this text instead of dropping it silently.
inline constexpr std::string_view no_fog_argument = "--eawr-no-fog";

[[nodiscard]] inline bool no_fog_requested(std::span<const std::string> arguments) noexcept {
    return std::find(arguments.begin(), arguments.end(), no_fog_argument) != arguments.end();
}

[[nodiscard]] inline const char* no_fog_multiplayer_refusal() noexcept {
    return "Fog of war: off (--eawr-no-fog) is single-player only; it is refused in a multiplayer game. "
           "Turn it off in the launch options and start again.";
}

// The refusal for a start that is, or may be, multiplayer; nullptr when the flag is absent or the game is single-player.
[[nodiscard]] inline const char* no_fog_refusal(bool requested, bool multiplayer) noexcept {
    return requested && multiplayer ? no_fog_multiplayer_refusal() : nullptr;
}

} // namespace eawr::viewer
