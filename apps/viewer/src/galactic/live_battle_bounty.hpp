#pragma once

#include "battle_dialog_model.hpp"
#include "eawr/platform/live_game_session.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace eawr::viewer::galactic {
// GHUD-S88/WGP-86: read the published local tally, without paying it again.
[[nodiscard]] inline std::optional<CampaignBounty> live_campaign_bounty(
    const platform::LiveGameFrame& frame, std::uint64_t conflict, sim::galactic::PlayerId local,
    const std::function<std::string(std::string_view)>& text) {
    const campaign::BattleResult* result = nullptr;
    if (frame.result && frame.result->conflict == conflict) result = &*frame.result;
    else if (frame.auto_resolve_outcome && frame.auto_resolve_outcome->result.conflict == conflict)
        result = &frame.auto_resolve_outcome->result;
    if (!result || !result->bounty) return std::nullopt;
    bool benefits = false;
    if (frame.latest && frame.latest->observer()) for (const auto& player : frame.latest->observer()->players)
        if (player.player == local) { benefits = player.benefits_from_corruption; break; }
    auto caption = text("TEXT_SUMMARY_BOUNTY_COLLECTED");
    // The sourced text uses one signed decimal conversion; replace it as data,
    // never pass a mod-authored string to a variadic printf function.
    const auto token = caption.find("%d");
    if (token != std::string::npos)
        caption.replace(token, 2, std::to_string(result->bounty->trunc_to_integer()));
    return CampaignBounty{result->bounty->raw() > 0, benefits, std::move(caption)};
}

// Used synchronously while projecting this frame; the caller retains the frame.
[[nodiscard]] inline std::function<std::optional<CampaignBounty>(std::uint64_t, sim::galactic::PlayerId)>
live_campaign_bounty_namer(const platform::LiveGameFrame& frame,
    std::function<std::string(std::string_view)> text) {
    return [&frame, text = std::move(text)](const auto conflict, const auto local) {
        return live_campaign_bounty(frame, conflict, local, text);
    };
}

} // namespace eawr::viewer::galactic
