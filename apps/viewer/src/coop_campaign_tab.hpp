#pragma once
#include <string_view>

namespace eawr::viewer {
// The tab only filters the catalog; the selected campaign remains in the shared room options.
inline bool campaign_in_tab(bool single_player_rules, bool coop_tab) noexcept {
    return single_player_rules == coop_tab;
}
inline bool room_campaign_tab(std::string_view campaign, std::string_view previous_campaign,
                              bool single_player_rules, bool current_tab, bool checkpoint_changed = false) noexcept {
    return campaign != previous_campaign || checkpoint_changed ? single_player_rules : current_tab;
}

// Which of the two tabs are drawn, and which one is selected. A tab with no campaign behind it is an empty box (the
// retail staging lists only the definitions whose Is_Multiplayer matches the mode, WGP-65, and the tab pair is ours), so it is left out and the
// other one is selected. With nothing to list at all both stay, as the list then says that nothing is available.
struct CampaignTabs {
    bool multiplayer_shown{true}, coop_shown{true}, coop_selected{};
};
inline CampaignTabs campaign_tabs(bool has_multiplayer, bool has_coop, bool coop_wanted) noexcept {
    if (has_multiplayer == has_coop) return {true, true, coop_wanted};
    return {has_multiplayer, has_coop, has_coop};
}

// The two campaign tabs above the list: the width of each (they fill `total`), from the width each caption needs.
// Halves by default; a caption wider than its half takes what it needs from the other tab, so no caption is clipped.
// When both captions cannot fit, the available width is shared in proportion to what they need.
struct CampaignTabWidths {
    float multiplayer{}, coop{};
};
inline CampaignTabWidths campaign_tab_widths(float total, float multiplayer_needed, float coop_needed) noexcept {
    if (total <= 0.0F) return {};
    const float half = total * 0.5F;
    if (multiplayer_needed + coop_needed > total) {
        const float share = multiplayer_needed / (multiplayer_needed + coop_needed);
        return {total * share, total - total * share};
    }
    if (multiplayer_needed > half) return {multiplayer_needed, total - multiplayer_needed};
    if (coop_needed > half) return {total - coop_needed, coop_needed};
    return {half, total - half};
}
// The empty strip left between the tabs and the first list row.
inline constexpr float campaign_tab_gap = 6.0F;
} // namespace eawr::viewer
