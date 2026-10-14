#include "eawr/platform/campaign_end_inputs.hpp"
#include "eawr/platform/campaign_end_provider.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "eawr/data/tag_trace.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eawr::platform {
namespace {
// An unresolved faction or an absent tag supplies nothing (WGP-97 shows the absent
// scoring half as `0%`); the end boundary itself must not fail on missing art.
std::optional<data::EffectiveObject> faction_object(const data::Catalog& catalog, std::string_view name) {
    if (name.empty()) return std::nullopt;
    auto resolved = catalog.resolve(name, data::Category::faction);
    if (!resolved) return std::nullopt;
    return std::move(resolved).value();
}
std::string authored(const std::optional<data::EffectiveObject>& faction, std::string_view tag) {
    if (!faction) return {};
    const auto* value = faction->value(tag);
    if (!value) return {};
    data::tag_trace::used(value->value);
    return value->value.raw_text;
}
} // namespace

core::Result<CampaignEndInputs> campaign_end_inputs_from_catalog(
    const data::Catalog& catalog, std::string_view local_faction, std::string_view winner_faction) {
    CampaignEndInputs inputs;
    const auto local = faction_object(catalog, local_faction);
    const auto winner = faction_object(catalog, winner_faction);
    inputs.victory_text = authored(local, "Victory_Text");
    inputs.defeat_text = authored(local, "Defeat_Text");
    inputs.space_win_image = authored(local, "Space_Win_Image");
    inputs.land_win_image = authored(local, "Land_Win_Image");
    inputs.space_lose_image = authored(local, "Space_Lose_Image");
    inputs.land_lose_image = authored(local, "Land_Lose_Image");
    inputs.generic_win_movie = authored(winner, "Generic_Win_Movie");
    inputs.superweapon_win_movie = authored(winner, "Superweapon_Win_Movie");
    inputs.finale_movie = authored(winner, "Finale_Movie");
    inputs.second_finale_movie = authored(winner, "Finale_Movie_2");
    inputs.post_credits_movie = authored(winner, "Post_Credits_Movie");
    return core::Result<CampaignEndInputs>::success(std::move(inputs));
}

core::Result<CampaignEndInputs> campaign_end_inputs_from_game(
    const data::Catalog& catalog, const campaign::GameSession& game,
    const sim::galactic::CampaignResult& result, sim::galactic::PlayerId local) {
    std::string local_faction, winner_faction;
    if (const auto& monitor = game.galaxy().victory()) for (const auto& row : monitor->context.players) {
        if (row.player == local) local_faction = row.faction;
        if (row.player == result.condition.player) winner_faction = row.faction;
    }
    return campaign_end_inputs_from_catalog(catalog, local_faction, winner_faction);
}

CampaignEndInputsProvider campaign_end_inputs_provider(
    std::shared_ptr<const data::Catalog> catalog, std::shared_ptr<const CampaignScoringSource> scoring) {
    return [catalog = std::move(catalog), scoring = std::move(scoring)](const campaign::GameSession& game,
        const sim::galactic::CampaignResult& result, sim::galactic::PlayerId local) -> core::Result<CampaignEndInputs> {
        auto inputs = campaign_end_inputs_from_game(*catalog, game, result, local);
        if (!inputs || !scoring) return inputs;
        auto value = std::move(inputs).value();
        std::vector<CampaignScoringPlayer> players;
        if (const auto& monitor = game.galaxy().victory())
            for (const auto& row : monitor->context.players) players.push_back({row.player, row.faction});
        auto outcome = evaluate_campaign_scoring(*scoring, game.galaxy().score_ledger(), players, local);
        if (!outcome) value.scoring_seam = "scoring module failed: " + outcome.error().message;
        else if (!outcome.value()) value.scoring_seam = "authored scoring module is absent";
        else {
            value.title_key = outcome.value()->title_key;
            value.efficiencies = outcome.value()->efficiencies;
        }
        return core::Result<CampaignEndInputs>::success(std::move(value));
    };
}

std::shared_ptr<const CampaignServiceConfiguration> campaign_end_services(
    std::shared_ptr<const data::Catalog> catalog, std::optional<SyntheticCampaignScoring> scoring) {
    auto services = std::make_shared<CampaignServiceConfiguration>();
    services->campaign_end_inputs = [catalog = std::move(catalog), scoring = std::move(scoring)](
        const campaign::GameSession& game, const sim::galactic::CampaignResult& result,
        sim::galactic::PlayerId local) -> core::Result<CampaignEndInputs> {
        auto inputs = campaign_end_inputs_from_game(*catalog, game, result, local);
        if (!inputs || !scoring) return inputs;
        auto value = std::move(inputs).value();
        value.title_key = scoring->title_key;
        value.efficiencies = scoring->efficiencies;
        return core::Result<CampaignEndInputs>::success(std::move(value));
    };
    return services;
}

std::shared_ptr<const CampaignServiceConfiguration> campaign_end_services_scored(
    std::shared_ptr<const data::Catalog> catalog, std::shared_ptr<const CampaignScoringSource> scoring) {
    auto services = std::make_shared<CampaignServiceConfiguration>();
    services->campaign_end_inputs = campaign_end_inputs_provider(std::move(catalog), std::move(scoring));
    return services;
}

} // namespace eawr::platform
