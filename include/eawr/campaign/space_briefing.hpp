#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::campaign {

// SND-41/49: authored rows retain XML order; C is an exclusion.
struct SpaceBriefingRow {
    std::uint64_t a{}, b{}, c{}; // None = 0
    std::string speech;
};
struct SpaceBriefingContent {
    std::vector<SpaceBriefingRow> either, both;
    std::string fallback, command_bar_movie;
};
struct SpaceBriefingGate {
    bool enabled{true}, parent_mode{true}, cinematic_playing{}, speech_queue_empty{true};
    std::uint64_t logical_frame{};
};
struct SpaceBriefingRequest { std::string speech, command_bar_movie; };
// Caller retains done across presentation reconstruction; missing/refused speech never retries.
[[nodiscard]] std::optional<SpaceBriefingRequest> service_space_briefing(bool& done,
    const SpaceBriefingGate& gate, const SpaceBriefingContent& content,
    std::span<const std::uint64_t> available_persistent_types);
struct SpaceIntroGate {
    bool in_game_cinematics{}, enabled{}, prebattle{}, delayed_arrival{},
        local_attacker{}, conflict_lead{}, network{};
};
[[nodiscard]] bool space_intro_allowed(const SpaceIntroGate& gate) noexcept;

} // namespace eawr::campaign
