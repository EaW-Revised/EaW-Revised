#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::viewer::galactic {

// WGUI-71/104, GFC-28: one hint producer entry per HUD mode (galactic, land, space). A fresh entry is enabled with its
// first deadline at mode frame 90 and index 0. Turning the entry off or on changes only `enabled`.
enum class HintMode : std::size_t { galactic = 0, land = 1, space = 2 };
struct HintEntry {
    bool enabled{true};
    std::uint64_t deadline{90};
    std::size_t index{};
};
using HintEntries = std::array<HintEntry, 3>;

// GFC-28: the interval in mode frames is the integer conversion of `Advisor_Hint_Interval` x logical rate + 0.5. A missing,
// negative or non-numeric value leaves the producer without an interval (no hints), never a substituted default.
[[nodiscard]] inline std::optional<std::uint64_t> hint_interval_frames(std::string_view authored, std::uint32_t logical_fps) {
    constexpr std::string_view blank = " \t\r\n";
    const auto first = authored.find_first_not_of(blank);
    if (first == std::string_view::npos) return std::nullopt;
    const auto last = authored.find_last_not_of(blank);
    const std::string text(authored.substr(first, last - first + 1));
    char* end{};
    const double seconds = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(seconds) || seconds < 0) return std::nullopt;
    const double frames = seconds * static_cast<double>(logical_fps) + 0.5;
    if (frames >= 1.8e19) return std::nullopt;
    return static_cast<std::uint64_t>(frames);
}

// A faction hint list is authored as text keys separated by commas and/or white space, in authored order.
[[nodiscard]] inline std::vector<std::string> hint_keys(std::string_view authored) {
    std::vector<std::string> keys;
    std::string current;
    for (const char c : authored) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!current.empty()) keys.push_back(std::move(current));
            current.clear();
        } else current.push_back(c);
    }
    if (!current.empty()) keys.push_back(std::move(current));
    return keys;
}

// WGUI-71/GFC-28: one due attempt per call. When the entry is enabled and the mode frame has reached its deadline, return
// the index to show, advance and wrap the index, and rearm from the current frame. The attempt counts whether or not the
// popup is later displayed. A disabled entry keeps its deadline and index, so re-enabling an overdue entry attempts at once.
[[nodiscard]] inline std::optional<std::size_t> due_hint(HintEntry& entry, std::uint64_t frame, std::size_t count,
    std::uint64_t interval) {
    if (!entry.enabled || count == 0 || frame < entry.deadline) return std::nullopt;
    const std::size_t shown = entry.index % count;
    entry.index = (shown + 1) % count;
    entry.deadline = frame + interval;
    return shown;
}

// WGP-125: the Droid Log checkbox starts as the OR of the three mode flags and sets all three to its value, keeping each
// deadline and index.
[[nodiscard]] inline bool any_hints_enabled(const HintEntries& entries) {
    for (const auto& entry : entries) if (entry.enabled) return true;
    return false;
}
inline void set_all_hints(HintEntries& entries, bool enabled) {
    for (auto& entry : entries) entry.enabled = enabled;
}

// Unverified (GFSC-25 not captured): the first frame a campaign timeline is seen past the fresh deadline (a restored
// save) waits the fresh 90-frame delay from that frame instead of firing at once.
inline void first_hint_frame(HintEntry& entry, std::uint64_t frame) {
    if (frame > entry.deadline) entry.deadline = frame + 90;
}
// A load in a running viewer session starts a new timeline: each entry's deadline and index are fresh again; the
// player's hints checkbox (the enabled flags) is kept.
inline void restart_hint_timeline(HintEntries& entries) {
    for (auto& entry : entries) entry = HintEntry{entry.enabled};
}

// WGUI-87/106, WGP-122: an accepted advisor popup appends three archive entries in order: the cycle line (the formatted
// `TEXT_LOG_CYCLE_01` with the fiscal cycle at submission, date colour), the message in its own colour, and
// `TEXT_HELP_SEPERATOR` in the separator colour. No dedup and no cap on this path.
using Rgba = std::array<std::uint8_t, 4>;
struct ArchiveEntry {
    std::string text;
    Rgba colour{255, 255, 255, 255};
};
inline void archive_submission(std::vector<ArchiveEntry>& archive, std::string cycle_text, Rgba date_colour,
    std::string message, Rgba message_colour, std::string separator, Rgba separator_colour) {
    archive.push_back({std::move(cycle_text), date_colour});
    archive.push_back({std::move(message), message_colour});
    archive.push_back({std::move(separator), separator_colour});
}

// An authored "R G B" (optionally "R G B A") colour; anything else is absent.
[[nodiscard]] inline std::optional<Rgba> authored_rgba(std::string_view authored) {
    Rgba out{0, 0, 0, 255};
    std::size_t read = 0;
    for (const auto& token : hint_keys(authored)) {
        int value{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (read == 4 || parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value < 0 || value > 255)
            return std::nullopt;
        out[read++] = static_cast<std::uint8_t>(value);
    }
    if (read < 3) return std::nullopt;
    return out;
}

} // namespace eawr::viewer::galactic
