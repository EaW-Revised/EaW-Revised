#pragma once

#include "eawr/platform/campaign_end_view.hpp"

#include <array>
#include <functional>
#include <memory>
#include <string>

namespace eawr::viewer::galactic {

// GHUD-S48/H120: the score dialog's texts, derived once from the copied end view
// (WGP-37/38/97, WGS-118). Pure data: the Godot adapter only places these in the
// authored IDD_CAMPAIGN_SCORE_DIALOG controls.
struct CampaignScoreCaptions {
    std::string heading_key;                 // IDC_VICTORY_STATIC
    std::string title_key;                   // IDC_TITLE_STATIC, a localization key (WGP-97)
    std::array<std::string, 3> efficiencies; // military, conquest, kill (WGP-97 request order)
    std::string list_key;                    // IDC_VICTORY_LIST: authored Victory_Text / Defeat_Text
    std::string picture;                     // IDC_BATTLE_VICTORY_IMAGE texture name
    std::string button_key;                  // IDC_QUIT_BUTTON
    friend bool operator==(const CampaignScoreCaptions&, const CampaignScoreCaptions&) = default;
};

// WGP-37: ordinary multiplayer results show nothing; only the score route or an
// explicit quit (WGS-118) opens the dialog.
[[nodiscard]] inline bool campaign_score_shown(const platform::CampaignEndView& view) {
    return view.route == platform::CampaignEndRoute::score || view.explicit_quit;
}

// `space_draw` is the presentation-stream 50/50 draw of WGP-38; the simulation
// never sees it.
[[nodiscard]] inline CampaignScoreCaptions campaign_score_captions(
    const platform::CampaignEndView& view, bool space_draw) {
    CampaignScoreCaptions captions;
    captions.heading_key = platform::campaign_heading(view);
    captions.list_key = platform::campaign_score_text(view);
    captions.picture = platform::campaign_score_picture(view, space_draw);
    // WGP-37: the quit button reads TEXT_BUTTON_MAIN_MENU, multiplayer TEXT_BUTTON_CONTINUE.
    captions.button_key = view.ending_mode == platform::CampaignEndingMode::solo
        ? "TEXT_BUTTON_MAIN_MENU" : "TEXT_BUTTON_CONTINUE";
    // GHUD-S48: absent stats zero. A missing provider is the same `0%` as an absent control.
    std::array<std::optional<double>, 3> values{};
    if (view.inputs) {
        captions.title_key = view.inputs->title_key;
        values = view.inputs->efficiencies;
    }
    for (std::size_t index = 0; index < values.size(); ++index)
        captions.efficiencies[index] = platform::campaign_efficiency_text(values[index]);
    return captions;
}

// WGP-111, WGS-116/117: Escape or the quit button only act while the score is the
// topmost dialog; the destination is the presentation callback's decision.
using CampaignScoreExit = std::function<void(platform::CampaignScoreDestination)>;

} // namespace eawr::viewer::galactic
