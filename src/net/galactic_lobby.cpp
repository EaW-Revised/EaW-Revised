#include "eawr/net/galactic_lobby.hpp"

#include <algorithm>
#include <cctype>
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
std::string human_sides_refusal(const std::size_t limit = galactic_lobby_max_human_sides) {
    if (limit < galactic_lobby_max_human_sides)
        return "This lobby window draws only " + std::to_string(limit) + " player rows for this campaign, so a campaign game here has at most "
            + std::to_string(limit) + " human sides.";
    return "A campaign game has at most " + std::to_string(galactic_lobby_max_human_sides) + " human sides.";
}
} // namespace

bool galactic_lobby_row_drawn(const gc::MpLobbyFaction& faction) noexcept {
    return !(faction.neutral || (!faction.playable && !faction.ai_controller));
}

std::size_t galactic_lobby_human_row_limit(const gc::MpLobbyCampaign& campaign, const std::size_t dialog_rows) noexcept {
    if (dialog_rows == 0) return galactic_lobby_max_human_sides;
    const auto drawn = static_cast<std::size_t>(std::count_if(campaign.factions.begin(), campaign.factions.end(),
        [](const gc::MpLobbyFaction& faction) { return galactic_lobby_row_drawn(faction); }));
    // Rows the dialog cannot draw are named by a line on the row after the last one drawn: that row is not a player's.
    const std::size_t rows = drawn > dialog_rows && dialog_rows >= 2U ? dialog_rows - 1U : dialog_rows;
    return std::min(rows, galactic_lobby_max_human_sides);
}

std::uint32_t galactic_lobby_max_humans(const gc::MpLobbyCampaign& campaign, const std::uint32_t seat_limit) noexcept {
    const auto playable = static_cast<std::uint32_t>(std::count_if(campaign.factions.begin(), campaign.factions.end(),
        [](const gc::MpLobbyFaction& faction) { return faction.playable && !faction.neutral; }));
    const auto ceiling = std::max<std::uint32_t>(2U, std::min<std::uint32_t>(seat_limit, static_cast<std::uint32_t>(gc::galactic_max_human_seats)));
    return campaign.single_player_rules ? ceiling : std::clamp<std::uint32_t>(playable, 2U, ceiling);
}

std::uint32_t galactic_lobby_default_humans(const gc::MpLobbyCampaign& campaign, const std::uint32_t seat_limit) noexcept {
    // Two for every campaign until a room can start with fewer humans than seats (#8130): a room waits for all the seats it opens with.
    return std::min<std::uint32_t>(2U, galactic_lobby_max_humans(campaign, seat_limit));
}

std::string galactic_room_waiting_text(const std::size_t seated, const std::uint32_t capacity) {
    if (seated >= capacity) return {};
    const auto missing = static_cast<std::size_t>(capacity) - seated;
    return "Waiting for " + std::to_string(missing) + (missing == 1 ? " more player" : " more players") + ": this room was opened for "
        + std::to_string(capacity) + ". Start stays off until it is full; open the room again with fewer Human players to start sooner.";
}

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
        || options.rows.size() < 2 || options.rows.size() > gc::galactic_max_players || options.auto_resolve > galactic_lobby_max_auto_resolve
        || options.vote_timeout > gc::mp_roster_max_vote_timeout_seconds)
        return refuse<LobbyGame>("Incomplete Galactic Conquest staging options.");
    std::ostringstream out; out.imbue(std::locale::classic());
    out << galactic_lobby_options_tag << galactic_lobby_options_version << ' ' << options.input_delay << ' ' << options.seed
        << ' ' << options.auto_resolve << ' ' << options.vote_timeout << ' ' << options.custom << ' ' << options.credits << ' ' << options.starting_tech
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
    if (!(in >> result.input_delay >> result.seed >> result.auto_resolve >> result.vote_timeout >> result.custom >> result.credits >> result.starting_tech
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

std::optional<GalacticAiCaption> galactic_ai_caption(std::string_view difficulty) noexcept {
    // Whole tokens only: the bare name or the stock "<name>_Default" (Easy_Default); Hardened_Mod and Normal_Brutal are a mod's own levels.
    const auto same = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
            [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
    };
    const auto starts = [&](std::string_view name) {
        return same(difficulty, name) || (difficulty.size() == name.size() + 8 && same(difficulty.substr(0, name.size()), name)
            && same(difficulty.substr(name.size()), "_default"));
    };
    if (starts("easy")) return GalacticAiCaption::easy;
    if (starts("normal") || starts("medium")) return GalacticAiCaption::normal;
    if (starts("hard")) return GalacticAiCaption::hard;
    return std::nullopt;
}

core::Result<GalacticLobbyOptions> default_galactic_lobby_options(const gc::MpLobbyCampaign& campaign,
    std::string_view difficulty, std::uint64_t seed, std::uint64_t input_delay, std::size_t planned_humans) {
    if (seed > std::numeric_limits<std::uint32_t>::max()) return refuse<GalacticLobbyOptions>(std::string(galactic_seed_too_wide));
    GalacticLobbyOptions options;
    // A difficulty the campaign's AI types do not author would be refused at Start, so a room starts at one they do.
    options.campaign = campaign.name;
    options.difficulty = gc::lobby_difficulty_offered(campaign, difficulty) ? std::string(difficulty) : gc::lobby_default_difficulty(campaign, difficulty);
    options.seed = seed; options.input_delay = input_delay;
    // The campaign's own constant stays the default when it states one; the host range bounds what a new room offers.
    if (campaign.vote_timeout_seconds) options.vote_timeout = galactic_lobby_clamp_vote_timeout(*campaign.vote_timeout_seconds);
    // D8: the host's own row is planned; the other playable rows start as AI factions a player takes. A co-op campaign is one side.
    const std::size_t wanted = campaign.single_player_rules ? 1U : std::clamp<std::size_t>(planned_humans, 1U, galactic_lobby_max_human_sides);
    std::size_t humans{};
    for (const auto& faction : campaign.factions) {
        GalacticLobbyRow row{faction.player, faction.faction_id, faction.flags, faction.name, false, galactic_lobby_default_dropout};
        if (faction.playable && !faction.neutral && humans < wanted) { row.human = true; ++humans; }
        options.rows.push_back(std::move(row));
    }
    if (humans == 0) return refuse<GalacticLobbyOptions>("Campaign " + campaign.name + " has no playable faction.");
    return core::Result<GalacticLobbyOptions>::success(std::move(options));
}

namespace {
// Everything that can be checked without the seats: the campaign, the rows against local content, the custom
// settings and the planned human sides.
core::Result<const gc::MpLobbyCampaign*> check_options(const LobbyGame& game, const GalacticLobbyOptions& options,
    std::span<const gc::MpLobbyCampaign> catalog, const std::size_t dialog_rows) {
    const auto* campaign = find_campaign(catalog, game.map);
    if (!campaign) return refuse<const gc::MpLobbyCampaign*>("Campaign " + game.map + " is not an eligible campaign installed here.");
    if (options.difficulty.empty() || options.difficulty.size() > gc::mp_roster_max_text)
        return refuse<const gc::MpLobbyCampaign*>("Choose a difficulty.");
    if (!gc::lobby_difficulty_offered(*campaign, options.difficulty)) {
        std::string offered;
        for (const auto& entry : campaign->difficulties) if (!entry.empty()) offered += (offered.empty() ? "" : ", ") + entry;
        return refuse<const gc::MpLobbyCampaign*>("The difficulty " + options.difficulty + " does not apply to campaign " + campaign->name
            + ". Choose one of: " + offered + ".");
    }
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
        return refuse<const gc::MpLobbyCampaign*>(human_sides_refusal());
    // A planned row the dialog cannot draw would hide its player (the dialog keeps its last row for the "+N AI factions" line).
    if (const auto limit = galactic_lobby_human_row_limit(*campaign, dialog_rows); human_rows > limit)
        return refuse<const gc::MpLobbyCampaign*>(human_sides_refusal(limit));
    if (campaign->single_player_rules && human_rows != 1)
        return refuse<const gc::MpLobbyCampaign*>("A single-player campaign is played by one co-op side.");
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

std::optional<GalacticLobbyOptions> galactic_lobby_options_with_seat_on(const GalacticLobbyOptions& options,
    const gc::MpLobbyCampaign& campaign, const LobbySnapshot& before, const PeerIndex peer, const gc::PlayerId slot,
    const std::size_t dialog_rows) {
    if (options.rows.size() != campaign.factions.size()) return std::nullopt;
    const auto target = std::find_if(options.rows.begin(), options.rows.end(), [&](const auto& row) { return row.player == slot; });
    if (target == options.rows.end()) return std::nullopt;
    const auto index = static_cast<std::size_t>(target - options.rows.begin());
    if (target->human) return options;
    if (!campaign.factions[index].playable) return std::nullopt;
    auto moved = options;
    moved.rows[index].human = true;
    const auto own = std::find_if(before.seats.begin(), before.seats.end(), [&](const auto& seat) { return seat.peer == peer; });
    if (own != before.seats.end() && own->selection.slot != slot) {
        const bool shared = std::any_of(before.seats.begin(), before.seats.end(), [&](const auto& seat) {
            return seat.peer != peer && seat.selection.slot == own->selection.slot; });
        if (!shared) for (auto& row : moved.rows) if (row.player == own->selection.slot) row.human = false;
    }
    const auto humans = static_cast<std::size_t>(std::count_if(moved.rows.begin(), moved.rows.end(), [](const auto& row) { return row.human; }));
    if (humans > galactic_lobby_human_row_limit(campaign, dialog_rows) || (campaign.single_player_rules && humans != 1)) return std::nullopt;
    return moved;
}

namespace {
// The candidate room with `moved` as its game: sides are the host's planned rows in player order, so a changed row set can
// renumber every seat's side and faction.
core::Result<void> apply_rows(LobbySnapshot& candidate, const GalacticLobbyOptions& moved) {
    auto encoded = encode_galactic_lobby_game(moved);
    if (!encoded) return core::Result<void>::failure(encoded.error());
    candidate.game = std::move(encoded).value();
    const auto sides = sides_of(moved);
    for (auto& value : candidate.seats) {
        const auto side = sides.find(value.selection.slot);
        const auto held = std::find_if(moved.rows.begin(), moved.rows.end(), [&](const auto& r) { return r.player == value.selection.slot; });
        if (side == sides.end() || held == moved.rows.end()) continue; // the validator names it
        value.selection.team = side->second; value.selection.faction = held->faction;
    }
    return core::Result<void>::success();
}
} // namespace

core::Result<void> follow_galactic_choice(const LobbySnapshot& before, LobbySnapshot& candidate, const PeerIndex peer,
    std::span<const gc::MpLobbyCampaign> catalog, const std::size_t dialog_rows) {
    if (lobby_game_mode(candidate.game) != LobbyGameMode::galactic) return core::Result<void>::success();
    auto decoded = decode_galactic_lobby_game(candidate.game);
    const auto* campaign = find_campaign(catalog, candidate.game.map);
    if (!decoded || !campaign) return core::Result<void>::success(); // the validator names it
    const auto seat = std::find_if(candidate.seats.begin(), candidate.seats.end(), [&](const auto& value) { return value.peer == peer; });
    if (seat == candidate.seats.end()) return core::Result<void>::success();
    const auto& options = decoded.value();
    const auto row = std::find_if(options.rows.begin(), options.rows.end(), [&](const auto& value) { return value.player == seat->selection.slot; });
    if (row == options.rows.end() || row->human) return core::Result<void>::success();
    const auto index = static_cast<std::size_t>(row - options.rows.begin());
    if (index >= campaign->factions.size() || !campaign->factions[index].playable) return core::Result<void>::success(); // the validator names it
    const auto moved = galactic_lobby_options_with_seat_on(options, *campaign, before, peer, seat->selection.slot, dialog_rows);
    if (!moved) return refuse(human_sides_refusal(galactic_lobby_human_row_limit(*campaign, dialog_rows))
        + " Choose one of the open factions, or ask the host to close a row first.");
    return apply_rows(candidate, *moved);
}

std::optional<gc::PlayerId> galactic_lobby_newcomer_row(const GalacticLobbyOptions& options, const gc::MpLobbyCampaign& campaign,
    const LobbySnapshot& room, const std::size_t dialog_rows) {
    if (options.rows.size() != campaign.factions.size()) return std::nullopt;
    const auto seats_on = [&](const gc::PlayerId player) {
        return std::count_if(room.seats.begin(), room.seats.end(), [&](const auto& seat) { return seat.selection.slot == player; });
    };
    std::size_t planned{};
    for (const auto& row : options.rows) {
        if (!row.human) continue;
        ++planned;
        if (seats_on(row.player) == 0) return row.player; // a row the host planned and nobody holds
    }
    if (planned == 0) return std::nullopt;
    if (!campaign.single_player_rules && planned < galactic_lobby_human_row_limit(campaign, dialog_rows)) {
        for (std::size_t index = 0; index < options.rows.size(); ++index) {
            const auto& faction = campaign.factions[index];
            if (!options.rows[index].human && faction.playable && !faction.neutral) return options.rows[index].player; // claimed by the newcomer
        }
    }
    const GalacticLobbyRow* best = nullptr;
    std::ptrdiff_t best_count = 0;
    for (const auto& row : options.rows) {
        if (!row.human) continue;
        const auto count = seats_on(row.player);
        if (!best || count < best_count) { best = &row; best_count = count; }
    }
    return best->player;
}

std::optional<GalacticLobbyReseat> galactic_lobby_reseat(GalacticLobbyOptions fresh, const gc::MpLobbyCampaign& campaign,
    const LobbySnapshot& room, const std::size_t dialog_rows) {
    const auto first_human = std::find_if(fresh.rows.begin(), fresh.rows.end(), [](const auto& row) { return row.human; });
    if (first_human == fresh.rows.end() || fresh.rows.size() != campaign.factions.size()) return std::nullopt;
    GalacticLobbyReseat result;
    LobbySnapshot placed = room; // the seats placed so far, on their new rows
    placed.seats.clear();
    for (const auto& seat : room.seats) {
        const auto row = seat.peer == room.host ? std::optional<gc::PlayerId>(first_human->player)
                                                : galactic_lobby_newcomer_row(fresh, campaign, placed, dialog_rows);
        if (!row) return std::nullopt;
        for (auto& value : fresh.rows) if (value.player == *row) value.human = true; // a claimed AI faction is planned with its seat
        auto moved = seat;
        moved.selection.slot = *row;
        placed.seats.push_back(std::move(moved));
        result.slots.push_back(*row);
    }
    result.options = std::move(fresh);
    return result;
}

std::optional<std::string> galactic_lobby_switch_refusal(const gc::MpLobbyCampaign& campaign, const LobbySnapshot& room) {
    if (room.seats.size() < 2U || campaign.single_player_rules) return std::nullopt;
    const auto playable = static_cast<std::size_t>(std::count_if(campaign.factions.begin(), campaign.factions.end(),
        [](const gc::MpLobbyFaction& faction) { return faction.playable && !faction.neutral; }));
    if (playable >= room.capacity) return std::nullopt;
    return "This room waits for " + std::to_string(room.capacity) + " players, but this campaign has only " + std::to_string(playable)
        + " factions to take, so it could never start. Open the room again to play it.";
}

void follow_galactic_departure(const LobbySnapshot& before, LobbySnapshot& after, const PeerIndex peer,
    std::span<const gc::MpLobbyCampaign> catalog) {
    if (lobby_game_mode(after.game) != LobbyGameMode::galactic) return;
    const auto left = std::find_if(before.seats.begin(), before.seats.end(), [&](const auto& seat) { return seat.peer == peer; });
    auto decoded = decode_galactic_lobby_game(after.game);
    const auto* campaign = find_campaign(catalog, after.game.map);
    if (left == before.seats.end() || !decoded || !campaign) return;
    auto options = std::move(decoded).value();
    const auto row = std::find_if(options.rows.begin(), options.rows.end(), [&](const auto& value) { return value.player == left->selection.slot; });
    if (row == options.rows.end() || !row->human) return;
    const bool held = std::any_of(after.seats.begin(), after.seats.end(), [&](const auto& seat) { return seat.selection.slot == row->player; });
    const auto planned = std::count_if(options.rows.begin(), options.rows.end(), [](const auto& value) { return value.human; });
    if (held || planned <= 1 || campaign->single_player_rules) return;
    row->human = false; // unclaimed: the row is an AI faction again
    auto candidate = after;
    if (!apply_rows(candidate, options)) return;
    after = std::move(candidate);
}

core::Result<void> validate_galactic_lobby(const LobbySnapshot& snapshot, bool starting, std::span<const gc::MpLobbyCampaign> catalog,
    const std::size_t dialog_rows) {
    auto options = decode_galactic_lobby_game(snapshot.game);
    if (!options) return core::Result<void>::failure(options.error());
    auto campaign = check_options(snapshot.game, options.value(), catalog, dialog_rows);
    if (!campaign) return core::Result<void>::failure(campaign.error());
    if (auto seats = check_seats(snapshot, options.value(), starting); !seats) return seats;
    if (starting) {
        // The human seat limit (D1): staging may hold a ninth peer, but Ready and Start say why it cannot play: with every side
        // at the limit no faction can take a second player (co-op seats only while sides x seats stay within it).
        if (snapshot.seats.size() > gc::galactic_max_human_seats)
            return refuse("A campaign game takes at most " + std::to_string(gc::galactic_max_human_seats)
                + " human seats, so once every faction has a player no faction can take a second one.");
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
    auto campaign = check_options(snapshot.game, options.value(), catalog, 0);
    if (!campaign) return Result::failure(campaign.error());
    if (auto seats = check_seats(snapshot, options.value(), false); !seats) return Result::failure(seats.error());
    const auto& value = options.value();
    const auto sides = sides_of(value);
    gc::MpRoster roster;
    roster.campaign = value.campaign; roster.difficulty = value.difficulty; roster.custom = value.custom;
    roster.credits = value.credits; roster.starting_tech = value.starting_tech; roster.maximum_tech = value.maximum_tech;
    roster.allow_heroes = value.allow_heroes; roster.vote_timeout_seconds = value.vote_timeout;
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

std::string galactic_prepare_wait_text(const bool host_waits, const std::uint64_t elapsed_seconds) {
    const std::string progress = " (" + std::to_string(elapsed_seconds) + " s).";
    if (!host_waits) return "Preparing your copy" + progress;
    return "Preparing the campaign" + progress + " Gives up after " + std::to_string(preparation_timeout_minutes)
        + (preparation_timeout_minutes == 1 ? " minute." : " minutes.");
}

} // namespace eawr::net
