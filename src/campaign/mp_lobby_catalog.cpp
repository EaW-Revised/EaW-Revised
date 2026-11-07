#include "eawr/campaign/mp_lobby_catalog.hpp"

#include <algorithm>
#include <iterator>
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
// Whether a single-player entry's Starting_Active_Player names a faction a player may hold. The test is the faction's
// own authored flags, so no faction or mod name appears here.
bool playable_starter(const data::galactic::GalacticData& data, const std::string& starter) {
    if (starter.empty()) return false;
    const auto wanted = upper(starter);
    return std::any_of(data.factions.begin(), data.factions.end(), [&](const auto& faction) {
        return upper(faction.name) == wanted && faction.is_playable.value_or(false) && !faction.is_neutral.value_or(false);
    });
}

// The difficulty entry per level (Easy, Normal, Hard) for the AI player types a campaign runs. Start reads the types of
// the AI seats only, finds the chosen entry in one of them, and refuses an entry that those types map to two levels. So:
//  - a faction that is never a player (any faction of a single-player entry but its starter, a multiplayer entry's
//    non-playable factions) always has an AI seat, and runs its AI_Player_Control type; neutral factions have none;
//  - a multiplayer entry's playable factions run their Basic_AI only when the host leaves them to the AI, which the
//    catalog cannot know: with no always-AI type, an entry must be carried by every such type to be safe;
//  - an entry any of those types maps to two levels is left out.
// Per level the entry most always-AI types share wins, the first seen on a tie. No type known: no list.
std::array<std::string, 3> campaign_difficulties(const data::galactic::GalacticData& data, const data::galactic::Campaign& campaign,
    const std::vector<sim::galactic::MpLobbyFaction>& factions, const bool multiplayer, const LobbyAiLevels& ai_levels) {
    std::array<std::string, 3> out;
    if (!ai_levels) return out;
    std::vector<std::string> certain, maybe;
    const auto add = [](std::vector<std::string>& types, const std::string& type) {
        if (!type.empty() && std::find(types.begin(), types.end(), upper(type)) == types.end()) types.push_back(upper(type));
    };
    for (const auto& faction : factions) {
        if (faction.neutral) continue;
        if (faction.playable) {
            // A single-player entry's starter is the human side; a multiplayer entry's playable row may or may not be.
            if (multiplayer)
                if (const auto* record = data.faction(faction.name)) add(maybe, record->basic_ai);
            continue;
        }
        const auto assigned = std::find_if(campaign.ai_player_control.begin(), campaign.ai_player_control.end(),
            [&](const auto& row) { return upper(row.faction) == upper(faction.name); });
        if (assigned != campaign.ai_player_control.end()) add(certain, assigned->name);
    }
    const auto& voters = certain.empty() ? maybe : certain;
    // Every voter must carry a value that is offered when the voters are the uncertain ones; one is enough otherwise.
    const std::size_t needed = certain.empty() ? maybe.size() : 1U;
    std::vector<std::string> all = certain;
    for (const auto& type : maybe) add(all, type);
    struct Entry { std::string name; std::array<std::size_t, 3> mapped{}, voted{}; };
    std::vector<Entry> entries;
    for (const auto& type : all) {
        const bool votes = std::find(voters.begin(), voters.end(), type) != voters.end();
        const auto levels = ai_levels(type);
        for (std::size_t level = 0; level < 3; ++level) {
            if (levels[level].empty()) continue;
            auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) { return upper(entry.name) == upper(levels[level]); });
            if (found == entries.end()) { entries.push_back({levels[level], {}, {}}); found = std::prev(entries.end()); }
            ++found->mapped[level];
            if (votes) ++found->voted[level];
        }
    }
    for (std::size_t level = 0; level < 3; ++level) {
        std::size_t best = 0;
        for (const auto& entry : entries) {
            const auto levels_used = std::count_if(entry.mapped.begin(), entry.mapped.end(), [](std::size_t count) { return count != 0; });
            if (needed == 0 || levels_used != 1 || entry.voted[level] < needed || entry.voted[level] <= best) continue;
            best = entry.voted[level];
            out[level] = entry.name;
        }
    }
    return out;
}
} // namespace

namespace {
std::vector<sim::galactic::MpLobbyCampaign> list_campaigns(const data::galactic::GalacticData& data, bool single_player_sets,
    const LobbyAiLevels& ai_levels) {
    std::vector<sim::galactic::MpLobbyCampaign> out;
    for (const auto& campaign : data.campaigns) {
        if (!campaign.is_listed.value_or(true) || campaign.tutorial.value_or(false)) continue;
        const bool multiplayer = campaign.is_multiplayer.value_or(false);
        if (multiplayer == single_player_sets) continue;
        // Co-op on a single-player entry (design D3): story campaigns stay out, and an entry without a human
        // faction has nothing to seat the players on: no starter, or a starter that is not a playable faction.
        if (!multiplayer && (campaign.is_story_campaign.value_or(false) || !playable_starter(data, campaign.starting_active_player))) continue;
        std::set<std::string> named;
        name_all(named, campaign.home_locations); name_all(named, campaign.starting_credits);
        name_all(named, campaign.starting_tech_levels); name_all(named, campaign.max_tech_levels);
        name_all(named, campaign.starting_forces); name_all(named, campaign.ai_player_control);
        sim::galactic::MpLobbyCampaign row;
        row.name = campaign.name;
        row.text_id = campaign.text_id;
        row.description_text = campaign.description_text;
        for (const auto& planet_name : campaign.planets) {
            const auto* planet = data.planet(planet_name);
            if (planet == nullptr || !planet->galactic_position) continue;
            row.planet_points.emplace_back(planet->galactic_position->x, planet->galactic_position->y);
        }
        row.single_player_rules = !multiplayer;
        row.supports_custom_settings = campaign.supports_custom_settings.value_or(false);
        row.minimum_custom_tech = campaign.minimum_custom_tech;
        row.maximum_custom_tech = campaign.maximum_custom_tech;
        sim::galactic::PlayerId next{};
        for (const auto& faction : data.factions) {
            const bool neutral = faction.is_neutral.value_or(false);
            if (!neutral && !named.contains(upper(faction.name))) continue;
            ++next;
            // A single-player entry names its human faction; every other faction is an AI seat.
            const bool playable = faction.is_playable.value_or(false) && !neutral
                && (multiplayer || upper(faction.name) == upper(campaign.starting_active_player));
            row.factions.push_back({next, next, 0, faction.name, playable, neutral});
        }
        // The starter must also be one of the entry's own factions, or no row could seat a player.
        if (!multiplayer && std::none_of(row.factions.begin(), row.factions.end(), [](const auto& faction) { return faction.playable; })) continue;
        row.difficulties = campaign_difficulties(data, campaign, row.factions, multiplayer, ai_levels);
        out.push_back(std::move(row));
    }
    return out;
}
} // namespace

std::vector<sim::galactic::MpLobbyCampaign> list_mp_lobby_campaigns(const data::galactic::GalacticData& data, const LobbyAiLevels& ai_levels) {
    return list_campaigns(data, false, ai_levels);
}
std::vector<sim::galactic::MpLobbyCampaign> list_coop_sp_lobby_campaigns(const data::galactic::GalacticData& data, const LobbyAiLevels& ai_levels) {
    return list_campaigns(data, true, ai_levels);
}
std::vector<sim::galactic::MpLobbyCampaign> list_all_lobby_campaigns(const data::galactic::GalacticData& data, const LobbyAiLevels& ai_levels) {
    auto out = list_campaigns(data, false, ai_levels);
    auto coop = list_campaigns(data, true, ai_levels);
    out.insert(out.end(), std::make_move_iterator(coop.begin()), std::make_move_iterator(coop.end()));
    return out;
}

const sim::galactic::MpLobbyCampaign* find_mp_lobby_campaign(const std::vector<sim::galactic::MpLobbyCampaign>& campaigns,
    std::string_view name) noexcept {
    const auto wanted = upper(name);
    for (const auto& campaign : campaigns) if (upper(campaign.name) == wanted) return &campaign;
    return nullptr;
}

const sim::galactic::MpLobbyCampaign* first_hostable_lobby_campaign(const std::vector<sim::galactic::MpLobbyCampaign>& campaigns) noexcept {
    for (const auto& campaign : campaigns)
        for (const auto& faction : campaign.factions)
            if (faction.playable && !faction.neutral) return &campaign;
    return nullptr;
}

std::string lobby_campaign_row_title(const sim::galactic::MpLobbyCampaign& campaign, const std::string_view title) {
    std::string out(title);
    if (!campaign.single_player_rules) return out;
    // The faction leads: the list cuts a long caption at its end, and the faction is what tells the rows of one set apart.
    for (const auto& faction : campaign.factions) {
        if (!faction.playable) continue;
        return faction.name + " - " + out;
    }
    return out;
}

MpLobbyCampaignLabels label_mp_lobby_campaign(const sim::galactic::MpLobbyCampaign& campaign, const MpLobbyTextResolver& resolve) {
    MpLobbyCampaignLabels out;
    out.title = campaign.name;
    const auto lookup = [&](const std::string& key) -> std::optional<std::string> {
        if (key.empty()) return std::nullopt;
        auto value = resolve(key);
        if (!value || value->empty()) out.missing.push_back(key);
        return value && !value->empty() ? value : std::nullopt;
    };
    if (auto title = lookup(campaign.text_id)) out.title = std::move(*title);
    if (auto description = lookup(campaign.description_text)) {
        // The text database writes a line break inside a description as the two characters backslash and n.
        for (std::size_t at = 0; (at = description->find("\\n", at)) != std::string::npos; ++at) description->replace(at, 2, "\n");
        out.description = std::move(*description);
    }
    return out;
}

} // namespace eawr::campaign
