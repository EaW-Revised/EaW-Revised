#pragma once

#include "eawr/sim/tactical/retreat.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace eawr::presentation::ui {

// The Faction tags of the space retreat presentation (docs/behaviour/gc-retreat-presentation.md, RP-01..RP-08).
// An empty event name is an authored blank: it plays nothing, and it still replaces a generic fallback (RP-05).
struct RetreatFactionPresentation {
    std::string begin;                       // Space_Retreat_Begin_SFXEvent
    std::string enemy_begin;                 // Space_Retreat_Enemy_Begin_SFXEvent
    std::string cancel;                      // Space_Retreat_Cancel_SFXEvent
    std::string not_allowed;                 // Space_Retreat_Not_Allowed_SFXEvent
    std::array<std::string, 3> reason;       // Space_Retreat_Not_Allowed_Reason_1..3_SFXEvent
    std::string countdown_text_id;           // Space_Retreat_Countdown_Text_ID
    std::array<std::uint8_t, 4> colour{255, 255, 255, 255}; // Space_Retreat_Countdown_Color_RGBA
};

// RP-02: the retreat start cue is heard by the retreating player (type cue of the first roster
// member that has one, else the faction begin cue) and, as the enemy cue, by that player's
// enemies. Both faction cues are the LOCAL player's faction. Allies hear nothing.
[[nodiscard]] inline std::string retreat_start_cue(const bool local_is_retreater, const bool local_is_enemy,
    const RetreatFactionPresentation& local_faction, const std::string_view type_cue) {
    if (local_is_retreater) return type_cue.empty() ? local_faction.begin : std::string(type_cue);
    if (local_is_enemy) return local_faction.enemy_begin;
    return {};
}

// RP-03: the type cue is only used for the retreating local player's own cancel (a player command);
// every other cancel, also an enemy's, plays the RETREATING player's faction cue.
[[nodiscard]] inline std::string retreat_cancel_cue(const bool local_player_command,
    const RetreatFactionPresentation& retreating_faction, const std::string_view type_cue) {
    if (local_player_command && !type_cue.empty()) return std::string(type_cue);
    return retreating_faction.cancel;
}

// RP-05: a refused local press plays the reason cue of the local faction when the refusal has one
// (even when that cue is blank), else the generic not-allowed cue. Refusals that never reach the
// command bar request play nothing.
[[nodiscard]] inline std::string retreat_refusal_cue(const sim::tactical::RetreatRefusal refusal,
    const RetreatFactionPresentation& local_faction) {
    using sim::tactical::RetreatRefusal;
    switch (refusal) {
    case RetreatRefusal::prevention: return local_faction.reason[1];   // reason 2: preventing hero ability
    case RetreatRefusal::superweapon: return local_faction.reason[2];  // reason 3: tactical superweapon
    case RetreatRefusal::lockout:
    case RetreatRefusal::other_player:
    case RetreatRefusal::forced:
    case RetreatRefusal::escape_started: return local_faction.not_allowed;
    default: return {};
    }
}

// RP-06: the countdown line is shown while a retreat is underway and no cinematic plays.
[[nodiscard]] inline bool retreat_countdown_visible(const sim::tactical::RetreatState& state, const bool cinematic_playing) {
    using sim::tactical::RetreatPhase;
    return (state.phase == RetreatPhase::countdown || state.phase == RetreatPhase::escaping) && !cinematic_playing;
}

// RP-07: the retreating player's faction text, a space, then whole seconds and thousandths, both
// zero-padded to three digits, from the 32-bit float quotient frames / logical FPS. Only the
// countdown phase has remaining time; the escape phase reads zero.
[[nodiscard]] inline std::string retreat_countdown_line(const std::string_view text,
    const sim::tactical::RetreatState& state, const std::uint64_t tick, const std::uint32_t logical_fps) {
    float seconds = 0.0F;
    if (state.phase == sim::tactical::RetreatPhase::countdown && logical_fps > 0) {
        const auto frames = state.deadline > tick ? state.deadline - tick : 0;
        seconds = static_cast<float>(static_cast<int>(frames)) / static_cast<float>(static_cast<int>(logical_fps));
    }
    const int whole = static_cast<int>(seconds);
    const int thousandths = static_cast<int>((seconds - static_cast<float>(whole)) * 1000.0F);
    const auto pad = [](const int value) {
        std::string digits = std::to_string(value);
        return std::string(digits.size() < 3 ? 3 - digits.size() : 0, '0') + digits;
    };
    return std::string(text) + " " + pad(whole) + "." + pad(thousandths);
}

} // namespace eawr::presentation::ui
