#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The tactical super weapon's presentation rules (docs/behaviour/space-tactical-superweapon.md, TSW-15). Pure helpers
// the viewer consumes; nothing here touches the simulation, so none of it moves a hash.
namespace eawr::presentation::space {

// The six faction speech lines of the approach (TSW-15): the move toward the target, the 60/30/15/5 second warnings
// and the arrival.
enum class TswSpeech : std::uint8_t { moving_into_range, range_60, range_30, range_15, range_05, moved_into_range };
inline constexpr std::size_t tsw_speech_count = 6;

// The speech line of a crossed ETA threshold (60, 30, 15 or 5 whole seconds); none for any other value.
[[nodiscard]] std::optional<TswSpeech> tsw_speech_for_threshold(std::uint32_t seconds) noexcept;

// The faction tag that names the line: `own` is the phrase for a super weapon whose owner is not an enemy of the local
// player (SpeechEvent_Super_Weapon_Moving_Range_60_Seconds), otherwise the enemy phrase (..._Enemy_Moving_...).
[[nodiscard]] std::string tsw_speech_tag(TswSpeech line, bool own);

// An object-name list tag (Death_Explosions, Death_By_TSW_Replacements): names split at commas, semicolons and
// whitespace, empties dropped, authored order kept.
[[nodiscard]] std::vector<std::string> tsw_split_names(std::string_view text);

// TSW-15: the target explosion is one entry of the prop type's Death_Explosions list, picked by the viewer's own
// random stream (never the simulation's): `draw` is a uniform value in [0, 2^32) from that stream. Empty list: none.
[[nodiscard]] std::optional<std::size_t> tsw_pick_explosion(std::size_t count, std::uint32_t draw) noexcept;

// TSW-15: the attack clip of the super weapon's model. The clip is `<model stem>_attack_00` (no XML tag names the
// index; unverified, the least visible choice: every stock model that has an attack clip has an index 0 one). It
// plays from the frame the sim publishes the attack start until the target's destruction (the sim's S8), and not
// before or after.
struct TswClipWindow final {
    std::uint64_t start_tick{};
    std::optional<std::uint64_t> end_tick; // the destruction tick, once seen
};

// Whether the clip is shown at `tick` and how many ticks into it that is (0 at the start tick): the start tick itself
// shows frame 0, the end tick no longer shows the clip.
// `tick` is the presentation clock in ticks (fractional between two).
[[nodiscard]] std::optional<double> tsw_clip_elapsed(const TswClipWindow& window, double tick) noexcept;

} // namespace eawr::presentation::space
