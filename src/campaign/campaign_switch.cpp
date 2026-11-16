#include "eawr/campaign/campaign_switch.hpp"

#include "eawr/campaign/mp_lobby_catalog.hpp"

#include <charconv>
#include <utility>

namespace eawr::campaign {
namespace {
std::string upper(std::string_view value) {
    std::string out(value);
    for (auto& c : out) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}
std::optional<std::int64_t> integer(std::string_view text) {
    std::int64_t value{};
    const auto end = text.data() + text.size();
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return value;
}
core::Result<CampaignSwitchTarget> refused(std::string reason) {
    core::Diagnostic error;
    error.code = std::string(campaign_switch_warning_code);
    error.severity = core::Severity::warning;
    error.message = std::move(reason);
    return core::Result<CampaignSwitchTarget>::failure(std::move(error));
}
bool playable(const data::galactic::Faction& faction) {
    return faction.is_playable.value_or(false) && !faction.is_neutral.value_or(false);
}
} // namespace

std::string campaign_switch_warning(const std::string_view reason) {
    return std::string(campaign_switch_warning_code) + " [warning]: LOAD_CAMPAIGN refused: " + std::string(reason);
}

core::Result<CampaignSwitchTarget> resolve_campaign_switch(const data::galactic::GalacticData& data,
    const CampaignSwitchRequest& request) {
    if (request.campaign.empty()) return refused("the reward names no campaign (Reward_Param1)");
    const auto* entry = data.campaign(request.campaign);
    if (entry == nullptr) return refused("unknown campaign " + request.campaign);
    const auto seated = local_start_faction(data, *entry);

    std::string side = seated;
    if (!request.side.empty()) {
        // Reward_Param2: an index in the order of the data's factions (the order the faction table is read in), or a name.
        if (const auto index = integer(request.side)) {
            if (*index < 0 || static_cast<std::uint64_t>(*index) >= data.factions.size())
                return refused("side " + request.side + " is not a faction index of this game's faction table");
            side = data.factions[static_cast<std::size_t>(*index)].name;
        } else {
            const auto* named = data.faction(request.side);
            if (named == nullptr) return refused("side " + request.side + " names no faction");
            side = named->name;
        }
    }
    if (side.empty()) return refused("campaign " + entry->name + " seats no player: it authors no playable starting faction");
    const auto* faction = data.faction(side);
    if (faction == nullptr || !playable(*faction))
        return refused("side " + side + " is not playable in campaign " + entry->name);
    if (upper(side) != upper(seated))
        return refused("side " + side + " is not playable in campaign " + entry->name
            + (seated.empty() ? std::string{} : " (a solo start seats its player on " + seated + ")"));

    CampaignSwitchTarget target;
    target.campaign = entry->name;
    target.faction = faction->name;
    // Reward_Param3: 0 easy, 1 normal, 2 hard (the index of the level); a name works too. Anything else is Normal.
    target.difficulty = "normal";
    if (const auto level = integer(request.difficulty)) {
        if (*level == 0) target.difficulty = "easy";
        else if (*level == 2) target.difficulty = "hard";
    } else if (const auto name = upper(request.difficulty); name == "EASY") target.difficulty = "easy";
    else if (name == "HARD") target.difficulty = "hard";
    if (const auto tech = integer(request.tech)) target.tech = *tech;
    return core::Result<CampaignSwitchTarget>::success(std::move(target));
}

} // namespace eawr::campaign
