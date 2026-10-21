#include "eawr/net/galactic_lobby.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <random>
#include <set>
#include <sstream>

namespace eawr::net {
namespace {
namespace gc = sim::galactic;

template<class T = void> core::Result<T> refuse(std::string message) {
    return core::Result<T>::failure(lobby_diagnostic(LobbyReason::invalid_choice, std::move(message)));
}
// "gc" + decimal version + space: the part of the tag a decoder may name when refusing an unknown version.
bool has_tag(std::string_view bytes, unsigned* version = nullptr) noexcept {
    if (bytes.substr(0, galactic_lobby_options_tag.size()) != galactic_lobby_options_tag) return false;
    std::size_t at = galactic_lobby_options_tag.size();
    unsigned value{};
    std::size_t digits{};
    while (at < bytes.size() && bytes[at] >= '0' && bytes[at] <= '9' && digits < 6) { value = value * 10U + static_cast<unsigned>(bytes[at] - '0'); ++at; ++digits; }
    if (digits == 0 || at >= bytes.size() || bytes[at] != ' ') return false;
    if (version) *version = value;
    return true;
}
const gc::MpLobbyCampaign* find_campaign(std::span<const gc::MpLobbyCampaign> catalog, std::string_view name) {
    for (const auto& campaign : catalog) if (campaign.name == name) return &campaign;
    return nullptr;
}
// Human sides are the host's human rows in ascending player order: side 1, side 2.
std::map<gc::PlayerId, gc::HumanSideId> sides_of(const GalacticLobbyOptions& options) {
    std::map<gc::PlayerId, gc::HumanSideId> sides;
    for (const auto& row : options.rows) if (row.human) sides.emplace(row.player, static_cast<gc::HumanSideId>(sides.size() + 1));
    return sides;
}
} // namespace

LobbyGameMode lobby_game_mode(const LobbyGame& game) noexcept {
    const std::string_view bytes(reinterpret_cast<const char*>(game.options.data()), game.options.size());
    return has_tag(bytes) ? LobbyGameMode::galactic : LobbyGameMode::skirmish;
}

std::optional<gc::HumanSideId> galactic_lobby_side(const GalacticLobbyOptions& options, gc::PlayerId player) noexcept {
    const auto sides = sides_of(options);
    const auto found = sides.find(player);
    if (found == sides.end()) return std::nullopt;
    return found->second;
}

namespace {
// The AI perception seed is 32-bit (the world-generation seed rule), so a session with a wider seed would refuse its
// AI setup after it had already started. Every entry point names the problem instead.
constexpr std::string_view galactic_seed_too_wide = "The Galactic Conquest seed must fit in 32 bits (qualified 32-bit AI seed).";
}  // namespace

core::Result<LobbyGame> encode_galactic_lobby_game(const GalacticLobbyOptions& options) {
    if (options.seed > std::numeric_limits<std::uint32_t>::max())
        return refuse<LobbyGame>(std::string(galactic_seed_too_wide));
    if (options.campaign.empty() || options.difficulty.empty() || options.input_delay == 0 || options.input_delay > max_input_delay
        || options.rows.size() < 2 || options.rows.size() > gc::galactic_max_players || options.auto_resolve > galactic_lobby_max_auto_resolve)
        return refuse<LobbyGame>("Incomplete Galactic Conquest staging options.");
    std::ostringstream out; out.imbue(std::locale::classic());
    out << galactic_lobby_options_tag << galactic_lobby_options_version << ' ' << options.input_delay << ' ' << options.seed
        << ' ' << options.auto_resolve << ' ' << options.custom << ' ' << options.credits << ' ' << options.starting_tech
        << ' ' << options.maximum_tech << ' ' << options.allow_heroes << ' ' << std::quoted(options.difficulty)
        << ' ' << options.rows.size();
    for (const auto& row : options.rows)
        out << ' ' << row.player << ' ' << row.faction_id << ' ' << row.flags << ' ' << std::quoted(row.faction)
            << ' ' << row.human << ' ' << static_cast<unsigned>(row.dropout);
    const auto bytes = out.str();
    if (bytes.size() > max_lobby_payload_bytes) return core::Result<LobbyGame>::failure(
        lobby_diagnostic(LobbyReason::resource_limit, "Staging options exceed the room budget."));
    return core::Result<LobbyGame>::success({options.campaign, {bytes.begin(), bytes.end()}});
}

core::Result<GalacticLobbyOptions> decode_galactic_lobby_game(const LobbyGame& game) {
    using Result = core::Result<GalacticLobbyOptions>;
    const auto bad = [] { return refuse<GalacticLobbyOptions>("Invalid canonical Galactic Conquest staging options."); };
    if (game.options.empty() || game.options.size() > max_lobby_payload_bytes) return bad();
    const std::string bytes(game.options.begin(), game.options.end());
    if (sanitize_lobby_text(bytes, bytes.size()) != bytes) return bad();
    unsigned version{};
    if (!has_tag(bytes, &version)) return bad();
    if (version != galactic_lobby_options_version)
        return refuse<GalacticLobbyOptions>("Unsupported Galactic Conquest staging options version " + std::to_string(version) + ".");
    std::istringstream in(bytes); in.imbue(std::locale::classic());
    std::string tag;
    in >> tag;
    GalacticLobbyOptions result;
    result.campaign = game.map;
    std::size_t count{};
    if (!(in >> result.input_delay >> result.seed >> result.auto_resolve >> result.custom >> result.credits >> result.starting_tech
        >> result.maximum_tech >> result.allow_heroes >> std::quoted(result.difficulty) >> count)
        || count < 2 || count > gc::galactic_max_players) return bad();
    if (result.seed > std::numeric_limits<std::uint32_t>::max()) return refuse<GalacticLobbyOptions>(std::string(galactic_seed_too_wide));
    for (std::size_t index = 0; index < count; ++index) {
        GalacticLobbyRow row; unsigned dropout{};
        if (!(in >> row.player >> row.faction_id >> row.flags >> std::quoted(row.faction) >> row.human >> dropout)
            || (dropout != static_cast<unsigned>(gc::FactionControl::ai) && dropout != static_cast<unsigned>(gc::FactionControl::passive))) return bad();
        row.dropout = static_cast<gc::FactionControl>(dropout);
        result.rows.push_back(std::move(row));
    }
    auto canonical = encode_galactic_lobby_game(result);
    if (!canonical || canonical.value() != game) return bad();
    return Result::success(std::move(result));
}

std::uint64_t draw_galactic_session_seed() {
    std::random_device device;
    return static_cast<std::uint32_t>(device());
}

core::Result<GalacticLobbyOptions> default_galactic_lobby_options(const gc::MpLobbyCampaign& campaign,
    std::string_view difficulty, std::uint64_t seed, std::uint64_t input_delay, bool two_sides) {
    if (seed > std::numeric_limits<std::uint32_t>::max()) return refuse<GalacticLobbyOptions>(std::string(galactic_seed_too_wide));
    GalacticLobbyOptions options;
    options.campaign = campaign.name; options.difficulty = std::string(difficulty);
    options.seed = seed; options.input_delay = input_delay;
    std::size_t humans{};
    for (const auto& faction : campaign.factions) {
        GalacticLobbyRow row{faction.player, faction.faction_id, faction.flags, faction.name, false, galactic_lobby_default_dropout};
        if (faction.playable && humans < (two_sides ? galactic_lobby_max_human_sides : 1U)) { row.human = true; ++humans; }
        options.rows.push_back(std::move(row));
    }
    if (humans == 0) return refuse<GalacticLobbyOptions>("Campaign " + campaign.name + " has no playable faction.");
    return core::Result<GalacticLobbyOptions>::success(std::move(options));
}

namespace {
// Everything that can be checked without the seats: the campaign, the rows against local content, the custom
// settings and the planned human sides.
core::Result<const gc::MpLobbyCampaign*> check_options(const LobbyGame& game, const GalacticLobbyOptions& options,
    std::span<const gc::MpLobbyCampaign> catalog) {
    const auto* campaign = find_campaign(catalog, game.map);
    if (!campaign) return refuse<const gc::MpLobbyCampaign*>("Campaign " + game.map + " is not a multiplayer campaign installed here.");
    if (options.difficulty.empty() || options.difficulty.size() > gc::mp_roster_max_text)
        return refuse<const gc::MpLobbyCampaign*>("Choose a difficulty.");
    if (options.rows.size() != campaign->factions.size()
        || !std::equal(options.rows.begin(), options.rows.end(), campaign->factions.begin(), [](const auto& row, const auto& faction) {
               return row.player == faction.player && row.faction_id == faction.faction_id && row.flags == faction.flags && row.faction == faction.name; }))
        return refuse<const gc::MpLobbyCampaign*>("The campaign's factions differ from the installed content.");
    if (options.custom) {
        if (!campaign->supports_custom_settings)
            return refuse<const gc::MpLobbyCampaign*>("This campaign does not support custom settings.");
        if (options.credits < 0 || options.starting_tech < 0 || options.maximum_tech < options.starting_tech
            || (campaign->minimum_custom_tech && options.starting_tech < *campaign->minimum_custom_tech)
            || (campaign->maximum_custom_tech && options.maximum_tech > *campaign->maximum_custom_tech))
            return refuse<const gc::MpLobbyCampaign*>("Custom credits or tech levels are out of range.");
    } else if (options.credits != 0 || options.starting_tech != 0 || options.maximum_tech != 0 || !options.allow_heroes) {
        return refuse<const gc::MpLobbyCampaign*>("Custom settings are set without the custom option.");
    }
    std::size_t human_rows{};
    for (std::size_t index = 0; index < options.rows.size(); ++index) {
        const auto& row = options.rows[index];
        if (!row.human) continue;
        ++human_rows;
        if (!campaign->factions[index].playable) return refuse<const gc::MpLobbyCampaign*>(row.faction + " cannot be held by a player.");
    }
    if (human_rows == 0) return refuse<const gc::MpLobbyCampaign*>("Plan at least one player faction.");
    if (human_rows > galactic_lobby_max_human_sides)
        return refuse<const gc::MpLobbyCampaign*>("A campaign game has at most two human sides.");
    return core::Result<const gc::MpLobbyCampaign*>::success(campaign);
}

core::Result<void> check_seats(const LobbySnapshot& snapshot, const GalacticLobbyOptions& options, bool starting) {
    const auto sides = sides_of(options);
    std::set<std::string> names;
    for (const auto& seat : snapshot.seats) {
        std::string name = seat.selection.name;
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (name.empty() || !names.insert(name).second) return refuse("Invalid or duplicate player choice.");
        const auto side = sides.find(seat.selection.slot);
        if (side == sides.end()) return refuse("Choose a faction a player may hold in this campaign.");
        const auto row = std::find_if(options.rows.begin(), options.rows.end(), [&](const auto& value) { return value.player == seat.selection.slot; });
        if (row->faction != seat.selection.faction || seat.selection.team != side->second)
            return refuse("The faction and side of a seat follow the faction row it holds.");
        if (starting && seat.player != seat.selection.slot) return refuse("Owner assignment is not committed.");
    }
    return core::Result<void>::success();
}
} // namespace

core::Result<void> validate_galactic_lobby(const LobbySnapshot& snapshot, bool starting, std::span<const gc::MpLobbyCampaign> catalog) {
    auto options = decode_galactic_lobby_game(snapshot.game);
    if (!options) return core::Result<void>::failure(options.error());
    auto campaign = check_options(snapshot.game, options.value(), catalog);
    if (!campaign) return core::Result<void>::failure(campaign.error());
    if (auto seats = check_seats(snapshot, options.value(), starting); !seats) return seats;
    if (starting) {
        auto draft = galactic_roster_draft(snapshot, catalog);
        if (!draft) return core::Result<void>::failure(draft.error());
        if (auto valid = gc::validate_mp_roster(draft.value(), false); !valid)
            return refuse(valid.error().message);
    }
    return core::Result<void>::success();
}

core::Result<gc::MpRoster> galactic_roster_draft(const LobbySnapshot& snapshot, std::span<const gc::MpLobbyCampaign> catalog) {
    using Result = core::Result<gc::MpRoster>;
    auto options = decode_galactic_lobby_game(snapshot.game);
    if (!options) return Result::failure(options.error());
    auto campaign = check_options(snapshot.game, options.value(), catalog);
    if (!campaign) return Result::failure(campaign.error());
    if (auto seats = check_seats(snapshot, options.value(), false); !seats) return Result::failure(seats.error());
    const auto& value = options.value();
    const auto sides = sides_of(value);
    gc::MpRoster roster;
    roster.campaign = value.campaign; roster.difficulty = value.difficulty; roster.custom = value.custom;
    roster.credits = value.credits; roster.starting_tech = value.starting_tech; roster.maximum_tech = value.maximum_tech;
    roster.allow_heroes = value.allow_heroes;
    for (const auto& row : value.rows) {
        gc::MpRosterPlayer player;
        player.player = row.player; player.faction_id = row.faction_id; player.flags = row.flags; player.faction = row.faction;
        player.dropout = row.dropout;
        roster.players.push_back(std::move(player));
    }
    auto ordered = snapshot.seats;
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.seat < b.seat; });
    std::set<gc::PlayerId> held;
    std::map<gc::HumanSideId, sim::SeatId> leads;
    sim::SeatId last{};
    std::uint32_t slot{};
    for (const auto& seat : ordered) {
        const auto side = sides.at(seat.selection.slot);
        roster.seats.push_back({seat.seat, seat.selection.slot, sim::SeatKind::human, slot++, true, side});
        held.insert(seat.selection.slot);
        leads.emplace(side, seat.seat); // the lowest seat of a side leads it
        last = seat.seat;
    }
    // AI seats follow the humans in ascending order. Neutral factions get none; every other unheld faction is AI (D2).
    std::uint32_t next = last.value;
    for (std::size_t index = 0; index < value.rows.size(); ++index) {
        const auto& faction = campaign.value()->factions[index];
        if (held.contains(faction.player) || faction.neutral) continue;
        roster.seats.push_back({sim::SeatId{++next}, faction.player, sim::SeatKind::ai, 0, true, 0});
    }
    for (const auto& [side, lead] : leads) roster.teams.push_back({side, lead, false});
    return Result::success(std::move(roster));
}

core::Result<SetupOffer> make_galactic_lobby_offer(const GalacticLobbyOptions& options, const gc::MpRoster& sealed,
    const sim::MatchAgreement& identity, const std::vector<SetupSeat>& seats) {
    GalacticSetupPayload payload;
    payload.roster = sealed;
    auto bytes = encode_galactic_setup(payload);
    if (!bytes) return core::Result<SetupOffer>::failure(bytes.error());
    SetupOffer offer;
    offer.domain = std::string(galactic_setup_domain);
    offer.setup = std::move(bytes).value();
    offer.seed = options.seed;
    offer.seats = seats;
    offer.input_delay = options.input_delay;
    offer.identity = identity;
    if (auto valid = validate_galactic_offer(offer); !valid) return core::Result<SetupOffer>::failure(valid.error());
    return core::Result<SetupOffer>::success(std::move(offer));
}

} // namespace eawr::net
