#include "eawr/presentation/space/tsw.hpp"

#include <array>
#include <cctype>

namespace eawr::presentation::space {

std::optional<TswSpeech> tsw_speech_for_threshold(const std::uint32_t seconds) noexcept {
    switch (seconds) {
    case 60: return TswSpeech::range_60;
    case 30: return TswSpeech::range_30;
    case 15: return TswSpeech::range_15;
    case 5: return TswSpeech::range_05;
    default: return std::nullopt;
    }
}

std::string tsw_speech_tag(const TswSpeech line, const bool own) {
    static constexpr std::array<std::string_view, tsw_speech_count> suffix{
        "Moving_Into_Range", "Moving_Range_60_Seconds", "Moving_Range_30_Seconds", "Moving_Range_15_Seconds",
        "Moving_Range_05_Seconds", "Moved_Into_Range"};
    std::string tag = own ? "SpeechEvent_Super_Weapon_" : "SpeechEvent_Super_Weapon_Enemy_";
    tag += suffix[static_cast<std::size_t>(line)];
    return tag;
}

std::vector<std::string> tsw_split_names(const std::string_view text) {
    std::vector<std::string> names;
    std::string current;
    const auto flush = [&] {
        if (!current.empty()) names.push_back(std::move(current));
        current.clear();
    };
    for (const char c : text) {
        if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c)) != 0) flush();
        else current.push_back(c);
    }
    flush();
    return names;
}

std::optional<std::size_t> tsw_pick_explosion(const std::size_t count, const std::uint32_t draw) noexcept {
    if (count == 0) return std::nullopt;
    return static_cast<std::size_t>((static_cast<std::uint64_t>(draw) * count) >> 32U);
}

std::optional<double> tsw_clip_elapsed(const TswClipWindow& window, const double tick) noexcept {
    if (tick < static_cast<double>(window.start_tick)) return std::nullopt;
    if (window.end_tick && tick >= static_cast<double>(*window.end_tick)) return std::nullopt;
    return tick - static_cast<double>(window.start_tick);
}

} // namespace eawr::presentation::space
