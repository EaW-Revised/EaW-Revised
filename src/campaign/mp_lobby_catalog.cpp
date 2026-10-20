#include "eawr/campaign/mp_lobby_catalog.hpp"

#include <algorithm>
#include <set>
#include <string>

namespace eawr::campaign {
namespace {
std::string upper(std::string_view value) {
    std::string out(value);
    for (auto& c : out) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}
void name_all(std::set<std::string>& names, const std::vector<data::galactic::FactionNumber>& rows) {
    for (const auto& row : rows) names.insert(upper(row.faction));
}
void name_all(std::set<std::string>& names, const std::vector<data::galactic::FactionName>& rows) {
    for (const auto& row : rows) names.insert(upper(row.faction));
}
void name_all(std::set<std::string>& names, const std::vector<data::galactic::PlacedType>& rows) {
    for (const auto& row : rows) names.insert(upper(row.faction));
}
} // namespace

std::vector<sim::galactic::MpLobbyCampaign> list_mp_lobby_campaigns(const data::galactic::GalacticData& data) {
    std::vector<sim::galactic::MpLobbyCampaign> out;
    for (const auto& campaign : data.campaigns) {
        if (!campaign.is_listed.value_or(true) || !campaign.is_multiplayer.value_or(false) || campaign.tutorial.value_or(false)) continue;
        std::set<std::string> named;
        name_all(named, campaign.home_locations); name_all(named, campaign.starting_credits);
        name_all(named, campaign.starting_tech_levels); name_all(named, campaign.max_tech_levels);
        name_all(named, campaign.starting_forces); name_all(named, campaign.ai_player_control);
        sim::galactic::MpLobbyCampaign row;
        row.name = campaign.name;
        row.supports_custom_settings = campaign.supports_custom_settings.value_or(false);
        row.minimum_custom_tech = campaign.minimum_custom_tech;
        row.maximum_custom_tech = campaign.maximum_custom_tech;
        sim::galactic::PlayerId next{};
        for (const auto& faction : data.factions) {
            const bool neutral = faction.is_neutral.value_or(false);
            if (!neutral && !named.contains(upper(faction.name))) continue;
            ++next;
            row.factions.push_back({next, next, 0, faction.name, faction.is_playable.value_or(false) && !neutral, neutral});
        }
        out.push_back(std::move(row));
    }
    return out;
}

const sim::galactic::MpLobbyCampaign* find_mp_lobby_campaign(const std::vector<sim::galactic::MpLobbyCampaign>& campaigns,
    std::string_view name) noexcept {
    const auto wanted = upper(name);
    for (const auto& campaign : campaigns) if (upper(campaign.name) == wanted) return &campaign;
    return nullptr;
}

} // namespace eawr::campaign
