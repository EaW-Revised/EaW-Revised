// GCMP-07 (#4363): the lobby's Galactic Conquest mode over the real lobby core and the #3768 setup offer. The
// options codec and its versioning, the campaign list, the staging refusals, and two-peer and nine-seat rooms that
// reach the tick-zero barrier on identities each peer derived from its own content. The skirmish options bytes stay
// pinned: a GC mode changes nothing about a skirmish room.
#include "setup_test_support.hpp"
#include "fake_transport.hpp"

#include "galactic_lobby_backend.hpp"
#include "lan_match.hpp"
#include "galactic_launch_options.hpp"
#include "eawr/platform/agreed_campaign.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "eawr/campaign/mp_agreement.hpp"
#include "eawr/campaign/mp_lobby_catalog.hpp"
#include "eawr/net/galactic_lobby.hpp"
#include "eawr/net/lobby_session.hpp"
#include "eawr/sim/tactical/victory.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "../support/env.hpp"
#include "../support/temp_dir.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
using namespace setup_test;
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace dg = eawr::data::galactic;
namespace viewer = eawr::presentation::godot_backend;
using Fixed = eawr::sim::math::Fixed;
using Digest = std::array<std::uint8_t, 32>;

Fixed whole(std::int64_t value) { return Fixed::from_integer(value).value(); }

struct Names {
    std::string a{"REBEL"}, b{"EMPIRE"}, ai{"UNDERWORLD"}, neutral{"NEUTRAL"};
};

// Two playable factions, one AI faction and a neutral one, plus campaigns the lobby must not list.
dg::GalacticData make_data(const Names& names) {
    dg::GalacticData d;
    for (const auto& name : {names.a, names.b, names.ai}) {
        dg::Faction row; row.name = name; row.is_neutral = false; row.is_playable = name == names.a || name == names.b;
        row.displayed_tech_level_adjustment = 0;
        row.multiplayer_campaign_heroes = {"Hero", "Hero"};
        d.factions.push_back(row);
    }
    dg::Faction neutral; neutral.name = names.neutral; neutral.is_neutral = true; neutral.displayed_tech_level_adjustment = 0;
    d.factions.push_back(neutral);
    dg::Planet planet; planet.name = "P"; planet.galactic_position = dg::Position{}; planet.behaviors = {"PLANET"};
    d.planets.push_back(planet);
    d.constants = {{"MaximumFleetMovementDistance", "10", whole(10)}, {"Fiscal_Cycle_Time_In_Secs", "45", whole(45)}};
    dg::Campaign campaign; campaign.name = "Multiplayer"; campaign.locations = {"P"}; campaign.planets = {"P"};
    campaign.supports_custom_settings = true; campaign.is_multiplayer = true;
    campaign.minimum_custom_tech = 1; campaign.maximum_custom_tech = 5;
    for (const auto& name : {names.a, names.b, names.ai}) {
        campaign.starting_credits.push_back({name, 9000}); campaign.starting_tech_levels.push_back({name, 2});
        campaign.max_tech_levels.push_back({name, 5}); campaign.home_locations.push_back({name, "P"});
    }
    d.campaigns.push_back(campaign);
    return d;
}
eawr::units::GalacticTypeTable make_types() {
    eawr::units::GalacticType ship; ship.name = "Ship"; ship.object_class = "SpaceUnit"; ship.can_hyperspace_without_ability = true;
    auto hero = ship; hero.name = "Hero";
    eawr::units::GalacticTypeTable table; table.types = {ship, hero};
    return table;
}
// What one peer owns: its installed content. Pointers into itself: never moved.
struct Local {
    dg::GalacticData data;
    eawr::units::GalacticTypeTable types;
    cp::MpLocalContent content;
    explicit Local(const Names& names = {}) : data(make_data(names)), types(make_types()) {
        content.data = &data; content.types = &types;
        content.script_manifest_digest = std::string(64, 'a'); content.xml_digest = std::string(64, 'b');
        content.command_quota = static_cast<std::uint32_t>(net::max_batch_commands);
        content.byte_quota = static_cast<std::uint32_t>(net::max_batch_bytes);
        content.ai_configuration = [](std::string_view faction, gc::PlayerId player, std::string_view) {
            std::string text = "ai:" + std::string(faction) + ":" + std::to_string(player);
            return eawr::core::Result<Bytes>::success(Bytes(text.begin(), text.end()));
        };
    }
    Local(const Local&) = delete;
    Local& operator=(const Local&) = delete;
    std::vector<gc::MpLobbyCampaign> catalog() const { return cp::list_mp_lobby_campaigns(data); }
};

constexpr std::uint64_t match_seed = 4242;
const char* const difficulty = "Normal_Default";

// ---- codec, versioning, mode ----

net::GalacticLobbyOptions two_side_options(const Local& local) {
    return require(net::default_galactic_lobby_options(local.catalog().front(), difficulty, match_seed));
}
net::GalacticLobbyOptions coop_options(const Local& local) {
    return require(net::default_galactic_lobby_options(local.catalog().front(), difficulty, match_seed, 3, false));
}

void codec_and_mode() {
    Local local;
    const auto options = two_side_options(local);
    const auto game = require(net::encode_galactic_lobby_game(options));
    expect(game.map == "Multiplayer", "the campaign is the room's map string");
    const std::string text(game.options.begin(), game.options.end());
    // The agreed bytes, pinned: any change to this text is a new version.
    expect(text == "gc1 3 4242 2 0 0 0 0 1 \"Normal_Default\" 4 1 1 0 \"REBEL\" 1 2 2 2 0 \"EMPIRE\" 1 2 3 3 0 \"UNDERWORLD\" 0 2 4 4 0 \"NEUTRAL\" 0 2",
        "gc1 options bytes: " + text);
    expect(net::lobby_game_mode(game) == net::LobbyGameMode::galactic, "the tag makes it a Galactic Conquest room");
    expect(require(net::decode_galactic_lobby_game(game)) == options, "options round trip");
    expect(require(net::encode_galactic_lobby_game(require(net::decode_galactic_lobby_game(game)))) == game, "re-encoding keeps the bytes");

    auto custom = options;
    custom.custom = true; custom.credits = 12000; custom.starting_tech = 2; custom.maximum_tech = 4; custom.allow_heroes = false;
    custom.auto_resolve = 3; custom.rows[1].dropout = gc::FactionControl::passive; custom.seed = 0xFFFFFFFFULL;
    expect(require(net::decode_galactic_lobby_game(require(net::encode_galactic_lobby_game(custom)))) == custom, "custom options round trip");

    // The AI perception seed is 32-bit: a wider seed is refused by name on every entry point.
    auto wide = options;
    wide.seed = (std::uint64_t{1} << 32U) + 5U;
    const auto wide_encoded = net::encode_galactic_lobby_game(wide);
    expect(!wide_encoded && wide_encoded.error().message.find("32 bits") != std::string::npos, "a seed above 32 bits is refused when encoding");
    const auto wide_default = net::default_galactic_lobby_options(local.catalog().front(), difficulty, wide.seed);
    expect(!wide_default && wide_default.error().message.find("32 bits") != std::string::npos, "default options refuse a seed above 32 bits");
    auto wide_game = game;
    const std::string narrow = std::to_string(options.seed), widened = std::to_string(wide.seed);
    std::string wide_bytes(wide_game.options.begin(), wide_game.options.end());
    wide_bytes.replace(wide_bytes.find(' ' + narrow + ' ') + 1, narrow.size(), widened);
    wide_game.options.assign(wide_bytes.begin(), wide_bytes.end());
    const auto wide_decoded = net::decode_galactic_lobby_game(wide_game);
    expect(!wide_decoded && wide_decoded.error().message.find("32 bits") != std::string::npos, "decoding refuses a seed above 32 bits by name");

    // Versioning: a newer tag is refused by name, never read as the current one.
    auto future = game;
    future.options[2] = '2';
    const auto refused = net::decode_galactic_lobby_game(future);
    expect(!refused && refused.error().message.find("version 2") != std::string::npos, "an unknown version is refused by name");
    expect(net::lobby_game_mode(future) == net::LobbyGameMode::galactic, "a future GC version is still a GC room");
    auto zero = game;
    zero.options[2] = '0';
    expect(!net::decode_galactic_lobby_game(zero), "version zero is refused");
    // Canonical bytes only.
    for (const auto& change : std::vector<std::function<void(net::LobbyGame&)>>{
             [](net::LobbyGame& g) { g.options.push_back(' '); },
             [](net::LobbyGame& g) { g.options.insert(g.options.begin() + 4, ' '); },
             [](net::LobbyGame& g) { g.options.push_back('0'); },
             [](net::LobbyGame& g) { g.options.insert(g.options.begin() + 4, '0'); },
             [](net::LobbyGame& g) { g.options.clear(); },
             [](net::LobbyGame& g) { g.options.assign({'g', 'c', '1'}); }})
    {
        auto changed = game;
        change(changed);
        expect(!net::decode_galactic_lobby_game(changed), "non-canonical option bytes are refused");
    }
    for (std::size_t size = 0; size < game.options.size(); ++size) {
        auto truncated = game;
        truncated.options.resize(size);
        expect(!net::decode_galactic_lobby_game(truncated), "a truncated option blob is refused at " + std::to_string(size));
    }
    // Encoder bounds.
    auto broken = options; broken.auto_resolve = 4;
    expect(!net::encode_galactic_lobby_game(broken), "auto-resolve is one of the four authored values");
    broken = options; broken.input_delay = 0;
    expect(!net::encode_galactic_lobby_game(broken), "an input delay is required");
    broken = options; broken.rows.resize(1);
    expect(!net::encode_galactic_lobby_game(broken), "a campaign has at least two factions");
    broken = options; broken.campaign.clear();
    expect(!net::encode_galactic_lobby_game(broken), "a campaign is named");
    auto bad_dropout = game;
    const std::string with_dropout = text;
    const auto at = with_dropout.rfind(" 2");
    bad_dropout.options.assign(with_dropout.begin(), with_dropout.begin() + static_cast<std::ptrdiff_t>(at));
    bad_dropout.options.push_back(' '); bad_dropout.options.push_back('9');
    expect(!net::decode_galactic_lobby_game(bad_dropout), "a dropout other than AI or passive is refused");

    // Mode detection never mistakes a skirmish room.
    net::LobbyGame skirmish{"data/art/maps/test.ted", {'1', ' ', '3', ' ', '0'}};
    expect(net::lobby_game_mode(skirmish) == net::LobbyGameMode::skirmish && !net::decode_galactic_lobby_game(skirmish),
        "a skirmish blob is not a GC room");
    expect(net::lobby_game_mode(net::LobbyGame{}) == net::LobbyGameMode::skirmish
        && net::lobby_game_mode(net::LobbyGame{"m", {'g', 'c'}}) == net::LobbyGameMode::skirmish
        && net::lobby_game_mode(net::LobbyGame{"m", {'g', 'c', 'x', ' '}}) == net::LobbyGameMode::skirmish, "no tag, no GC mode");
}

void skirmish_bytes_pinned() {
    eawr::skirmish::FixtureOptions options;
    options.map = "data/art/maps/test.ted"; options.seed = 7; options.victory_condition = eawr::sim::tactical::VictoryCondition::enemy_starbase_destroyed;
    options.match.emplace();
    auto& m = *options.match;
    m.allow_heroes = true; m.allow_superweapons = false; m.free_starting_units = true; m.pre_built_base = false; m.allow_random_events = true;
    m.credits = Fixed::from_raw(12345); m.start_tech = 2; m.max_tech = 5; m.game_timer = 30; m.win_integer = 3; m.auto_resolve = 2;
    m.win_float = Fixed::from_raw(99); m.win_condition = "WC"; m.space_win_condition = "SW";
    options.slots = std::vector<eawr::skirmish::LobbySlot>{{1, "Rebel", 1, true, {}, 0}, {2, "Empire", 2, false, {}, 1}};
    const auto game = require(viewer::encode_lan_lobby_game(options, 3));
    const std::string text(game.options.begin(), game.options.end());
    expect(text == "3 3 1 7 1 1 1 0 1 0 1 12345 2 5 30 3 2 99 \"WC\" \"SW\" 2 1 \"Rebel\" 1 2 0 0 0 2 \"Empire\" 2 3 0 1 0 0",
        "skirmish options text v3 includes each row's AI level: " + text);
    expect(game.map == "data/art/maps/test.ted" && net::lobby_game_mode(game) == net::LobbyGameMode::skirmish,
        "a skirmish room stays a skirmish room");
    expect(!net::decode_galactic_lobby_game(game), "the GC codec never reads skirmish bytes");
    net::LobbySnapshot snapshot; snapshot.game = game;
    expect(viewer::decode_lan_lobby_game(snapshot).has_value(), "the skirmish codec still reads its own bytes");
    auto galactic = require(net::encode_galactic_lobby_game(two_side_options(Local{})));
    snapshot.game = galactic;
    expect(!viewer::decode_lan_lobby_game(snapshot), "the skirmish codec refuses GC bytes");
}

// ---- the campaign list ----

void campaign_list() {
    Local local;
    auto& data = local.data;
    const auto base = data.campaigns.front();
    auto add = [&](const std::string& name, auto change) { auto row = base; row.name = name; change(row); data.campaigns.push_back(row); };
    add("Solo", [](dg::Campaign& c) { c.is_multiplayer = false; });
    add("Unset", [](dg::Campaign& c) { c.is_multiplayer.reset(); });
    add("Hidden", [](dg::Campaign& c) { c.is_listed = false; });
    add("Tutorial", [](dg::Campaign& c) { c.tutorial = true; });
    add("Second", [](dg::Campaign& c) { c.supports_custom_settings = false; c.minimum_custom_tech.reset(); c.maximum_custom_tech.reset(); });
    const auto list = cp::list_mp_lobby_campaigns(data);
    expect(list.size() == 2 && list[0].name == "Multiplayer" && list[1].name == "Second",
        "only Is_Listed + Is_Multiplayer campaigns, registry order, no tutorial");
    expect(cp::find_mp_lobby_campaign(list, "multiplayer") == &list[0] && !cp::find_mp_lobby_campaign(list, "Solo"), "lookup is case-insensitive and filtered");
    const auto& first = list[0];
    expect(first.supports_custom_settings && first.minimum_custom_tech == 1 && first.maximum_custom_tech == 5 && !list[1].supports_custom_settings,
        "custom settings and their limits come from the campaign");
    expect(first.factions.size() == 4 && first.factions[0] == gc::MpLobbyFaction{1, 1, 0, "REBEL", true, false}
        && first.factions[2] == gc::MpLobbyFaction{3, 3, 0, "UNDERWORLD", false, false}
        && first.factions[3] == gc::MpLobbyFaction{4, 4, 0, "NEUTRAL", false, true},
        "faction rows follow the data's faction order; the neutral faction is listed and unplayable");
    // A mod that renames and adds factions needs no code change.
    Local modded(Names{"CORPORATE_SECTOR", "ZANN_CONSORTIUM", "HUTT_CARTEL", "UNCLAIMED"});
    const auto modded_list = modded.catalog();
    expect(modded_list.size() == 1 && modded_list[0].factions[1].name == "ZANN_CONSORTIUM" && modded_list[0].factions[3].neutral,
        "renamed factions are listed as authored");
    auto extra = modded.data.factions[2];
    extra.name = "BLACK_SUN";
    modded.data.factions.insert(modded.data.factions.begin() + 3, extra);
    modded.data.campaigns[0].home_locations.push_back({"BLACK_SUN", "P"});
    expect(cp::list_mp_lobby_campaigns(modded.data)[0].factions.size() == 5, "a faction the campaign's records name is listed, however many there are");
    modded.data.campaigns[0].home_locations.pop_back();
    expect(cp::list_mp_lobby_campaigns(modded.data)[0].factions.size() == 4, "a faction no campaign record names is not listed");
}

// ---- staging validation, on hand-built room snapshots ----

net::LobbySeat seat_of(std::uint32_t peer, std::uint32_t id, const net::GalacticLobbyOptions& options, gc::PlayerId row, const std::string& name) {
    const auto found = std::find_if(options.rows.begin(), options.rows.end(), [&](const auto& value) { return value.player == row; });
    std::uint32_t side{};
    for (const auto& value : options.rows) if (value.human) { ++side; if (value.player == row) break; }
    return {peer, sim::SeatId{id}, row, {name, found->faction, 0, side, row}, false};
}
net::LobbySnapshot snapshot_of(const net::GalacticLobbyOptions& options, std::vector<net::LobbySeat> seats) {
    net::LobbySnapshot snapshot;
    snapshot.game = require(net::encode_galactic_lobby_game(options));
    snapshot.seats = std::move(seats);
    return snapshot;
}
bool refused(const net::LobbySnapshot& snapshot, bool starting, const std::vector<gc::MpLobbyCampaign>& catalog, const std::string& fragment = {}) {
    const auto result = net::validate_galactic_lobby(snapshot, starting, catalog);
    if (result) return false;
    return fragment.empty() || result.error().message.find(fragment) != std::string::npos;
}

void staging_validation() {
    Local local;
    const auto catalog = local.catalog();
    const auto two = two_side_options(local);
    const auto coop = coop_options(local);
    const auto good = snapshot_of(two, {seat_of(0, 1, two, 1, "host"), seat_of(1, 2, two, 2, "guest")});
    expect(net::validate_galactic_lobby(good, true, catalog).has_value(), "two human sides validate at Start");
    expect(net::validate_galactic_lobby(snapshot_of(coop, {seat_of(0, 1, coop, 1, "host")}), false, catalog).has_value(),
        "editable staging may be incomplete");
    expect(refused(snapshot_of(coop, {seat_of(0, 1, coop, 1, "host")}), true, catalog, "two human seats"), "one human is not a multiplayer game");

    // Not a multiplayer campaign here: the catalog is the Is_Listed + Is_Multiplayer list.
    auto solo = good; solo.game.map = "Solo";
    expect(refused(solo, false, catalog, "multiplayer campaign"), "a campaign outside the multiplayer list is refused");
    auto missing = good; missing.game.map = "Nowhere";
    expect(refused(missing, false, catalog), "an unknown campaign is refused");
    // Rows must be this peer's own local content.
    Local other(Names{"A", "B", "C", "D"});
    expect(refused(good, false, other.catalog(), "differ from the installed content"), "another peer's campaign data is refused");
    auto renamed = local.catalog(); renamed[0].factions[1].name = "EMPIRE_B";
    expect(refused(good, false, renamed, "differ from the installed content"), "faction rows that differ from local content are refused");
    // Planned rows.
    auto third = two;
    third.rows[2].human = true;
    expect(refused(snapshot_of(third, {seat_of(0, 1, third, 1, "host")}), false, catalog, "cannot be held"), "a human row on an unplayable faction is refused");
    auto neutral = two; neutral.rows[3].human = true; neutral.rows[1].human = false;
    expect(refused(snapshot_of(neutral, {}), false, catalog, "cannot be held"), "a neutral faction cannot be held by a player");
    auto no_human = two; no_human.rows[0].human = no_human.rows[1].human = false;
    expect(refused(snapshot_of(no_human, {}), false, catalog, "at least one"), "a room plans at least one player faction");
    // A third human side: three planned rows. Needs a third playable faction.
    Local three_factions;
    three_factions.data.factions[2].is_playable = true;
    const auto three_catalog = three_factions.catalog();
    auto three = require(net::default_galactic_lobby_options(three_catalog.front(), difficulty, match_seed));
    three.rows[2].human = true;
    expect(refused(snapshot_of(three, {}), false, three_catalog, "two human sides"), "a third human side is refused");
    // Seats.
    expect(refused(snapshot_of(two, {seat_of(0, 1, two, 1, "host"), seat_of(1, 2, two, 3, "guest")}), false, catalog), "a seat on a row no human may hold is refused");
    auto wrong_side = seat_of(1, 2, two, 2, "guest"); wrong_side.selection.team = 1;
    expect(refused(snapshot_of(two, {seat_of(0, 1, two, 1, "host"), wrong_side}), false, catalog, "follow the faction row"), "a seat's side follows its row");
    auto wrong_faction = seat_of(1, 2, two, 2, "guest"); wrong_faction.selection.faction = "REBEL";
    expect(refused(snapshot_of(two, {seat_of(0, 1, two, 1, "host"), wrong_faction}), false, catalog, "follow the faction row"), "a seat's faction follows its row");
    expect(refused(snapshot_of(two, {seat_of(0, 1, two, 1, "host"), seat_of(1, 2, two, 2, "HOST")}), false, catalog, "duplicate"), "player names are unique, case-insensitively");
    auto uncommitted = good; uncommitted.seats[1].player = 1;
    expect(net::validate_galactic_lobby(uncommitted, false, catalog) && refused(uncommitted, true, catalog, "not committed"), "the owner mapping must be committed at Start");
    // Two planned rows are two places a player may sit; both seats on one row is a co-op game and the other faction is AI.
    const auto shared = snapshot_of(two, {seat_of(0, 1, two, 1, "host"), seat_of(1, 2, two, 1, "guest")});
    expect(net::validate_galactic_lobby(shared, true, catalog).has_value(), "both players on one planned row validate at Start");
    const auto shared_draft = require(net::galactic_roster_draft(shared, catalog));
    expect(shared_draft.teams.size() == 1 && shared_draft.seats.size() == 4 && shared_draft.seats[2].kind == sim::SeatKind::ai
        && shared_draft.seats[2].player == 2 && shared_draft.seats[3].player == 3, "the unclaimed planned faction becomes an AI seat");
    // Co-op: up to eight humans on one side, a ninth is refused with the offer's own rule.
    std::vector<net::LobbySeat> eight, nine;
    for (std::uint32_t peer = 0; peer < 9; ++peer) {
        nine.push_back(seat_of(peer, peer + 1, coop, 1, "p" + std::to_string(peer)));
        if (peer < 8) eight.push_back(nine.back());
    }
    expect(net::validate_galactic_lobby(snapshot_of(coop, eight), true, catalog).has_value(), "eight humans on one side validate");
    expect(refused(snapshot_of(coop, nine), true, catalog, "at most 8 human seats"), "a ninth human is refused");
    expect(net::validate_galactic_lobby(snapshot_of(coop, nine), false, catalog).has_value(), "staging with nine seats is editable; Start refuses");
    // Options.
    auto custom = two; custom.custom = true; custom.credits = 5000; custom.starting_tech = 2; custom.maximum_tech = 4;
    expect(net::validate_galactic_lobby(snapshot_of(custom, {seat_of(0, 1, custom, 1, "host"), seat_of(1, 2, custom, 2, "guest")}), true, catalog).has_value(),
        "custom credits and tech inside the campaign limits");
    auto too_high = custom; too_high.maximum_tech = 6;
    expect(refused(snapshot_of(too_high, {}), false, catalog, "out of range"), "custom tech above the campaign limit is refused");
    auto too_low = custom; too_low.starting_tech = 0;
    expect(refused(snapshot_of(too_low, {}), false, catalog, "out of range"), "custom tech below the campaign limit is refused");
    auto inverted = custom; inverted.maximum_tech = 1;
    expect(refused(snapshot_of(inverted, {}), false, catalog, "out of range"), "maximum tech below starting tech is refused");
    auto stray = two; stray.credits = 100;
    expect(refused(snapshot_of(stray, {}), false, catalog, "custom"), "custom values without the custom option are refused");
    auto no_heroes = two; no_heroes.allow_heroes = false;
    expect(refused(snapshot_of(no_heroes, {}), false, catalog), "heroes off is a custom setting");
    custom.allow_heroes = false;
    expect(net::validate_galactic_lobby(snapshot_of(custom, {seat_of(0, 1, custom, 1, "host"), seat_of(1, 2, custom, 2, "guest")}), true, catalog).has_value(),
        "custom heroes off validates (WGS-14)");
    Local no_custom;
    no_custom.data.campaigns[0].supports_custom_settings = false;
    expect(refused(snapshot_of(custom, {}), false, no_custom.catalog(), "does not support"), "a campaign without custom settings refuses them");
    expect(refused(snapshot_of(two, {}), false, catalog) == false, "options alone validate");
}

// ---- the roster the agreed choices describe ----

void roster_draft() {
    Local local;
    const auto catalog = local.catalog();
    const auto two = two_side_options(local);
    const auto draft = require(net::galactic_roster_draft(snapshot_of(two, {seat_of(0, 1, two, 1, "host"), seat_of(1, 2, two, 2, "guest")}), catalog));
    expect(draft.campaign == "Multiplayer" && draft.difficulty == difficulty && !draft.custom && draft.allow_heroes, "campaign and difficulty carried");
    expect(draft.players.size() == 4 && draft.players[0].faction == "REBEL" && draft.players[3].faction == "NEUTRAL"
        && draft.players[1].dropout == net::galactic_lobby_default_dropout && draft.players[0].ai_configuration.empty(),
        "every campaign faction is a player; claims are filled in at sealing");
    expect(draft.seats.size() == 3 && draft.seats[0].kind == sim::SeatKind::human && draft.seats[0].seat_id.value == 1 && draft.seats[0].human_side_id == 1
        && draft.seats[1].human_side_id == 2 && draft.seats[1].player == 2
        && draft.seats[2].kind == sim::SeatKind::ai && draft.seats[2].player == 3 && draft.seats[2].seat_id.value == 3 && draft.seats[2].human_side_id == 0,
        "humans on their rows and sides, one AI seat for the unheld faction, none for the neutral one");
    expect(draft.teams.size() == 2 && draft.teams[0].lead.value == 1 && draft.teams[1].lead.value == 2, "each side's first seat leads it");
    expect(gc::validate_mp_roster(draft, false).has_value(), "the draft is a roster the offer accepts");

    const auto coop = coop_options(local);
    std::vector<net::LobbySeat> seats;
    for (std::uint32_t peer = 0; peer < 8; ++peer) seats.push_back(seat_of(peer, peer + 1, coop, 1, "p" + std::to_string(peer)));
    std::reverse(seats.begin(), seats.end());
    const auto eight = require(net::galactic_roster_draft(snapshot_of(coop, seats), catalog));
    expect(eight.seats.size() == 10 && eight.teams.size() == 1 && eight.teams[0].lead.value == 1
        && eight.seats[0].seat_id.value == 1 && eight.seats[7].seat_id.value == 8 && eight.seats[8].kind == sim::SeatKind::ai
        && eight.seats[8].seat_id.value == 9 && eight.seats[9].seat_id.value == 10,
        "eight humans on one side, lowest seat leads, AI seats follow in ascending order");
    expect(gc::validate_mp_roster(eight, false).has_value(), "the co-op draft is a roster the offer accepts");
    // Sparse seat IDs (a departed guest): AI seats still follow the highest human seat.
    const auto sparse = require(net::galactic_roster_draft(snapshot_of(two, {seat_of(0, 1, two, 1, "host"), seat_of(1, 30, two, 2, "guest")}), catalog));
    expect(sparse.seats[1].seat_id.value == 30 && sparse.seats[2].seat_id.value == 31 && gc::validate_mp_roster(sparse, false).has_value(),
        "seat IDs stay strictly ascending when guests came and went");
}

// ---- a real room, a real offer, the tick-zero barrier ----

class TickZeroAdapter final : public net::SessionAdapter {
public:
    explicit TickZeroAdapter(std::shared_ptr<cp::GameSession> session) : session_(std::move(session)) {}
    std::uint64_t completed_tick() const override { return 0; }
    std::string state_hash() const override { return session_->state_sha256(); }
    Bytes canonical_state() const override { const auto hash = state_hash(); return Bytes(hash.begin(), hash.end()); }
    eawr::core::Result<void> validate(const net::SeatInput&) const override { return unavailable<void>(); }
    eawr::core::Result<void> install(std::span<const net::SeatInput>) override { return unavailable<void>(); }
    eawr::core::Result<Bytes> canonical_input() const override { return eawr::core::Result<Bytes>::success(Bytes{}); }
    eawr::core::Result<net::CompletedTick> step() override { return unavailable<net::CompletedTick>(); }
    bool is_departure(const net::SeatInput&) const override { return false; }
private:
    template<class T> static eawr::core::Result<T> unavailable() {
        eawr::core::Diagnostic diagnostic; diagnostic.code = "EAWR-TEST-0001";
        diagnostic.message = "the galactic lockstep adapter is GCMP #3769";
        return eawr::core::Result<T>::failure(std::move(diagnostic));
    }
    std::shared_ptr<cp::GameSession> session_;
};

struct PeerState {
    std::shared_ptr<Local> local;
    std::unique_ptr<viewer::GalacticLobbyBackend> backend; // the code the viewer's lobby calls, on this peer's content
    int builds{};
    int contexts{};
    std::shared_ptr<const cp::MpAgreement> agreement() const { return backend->agreement(); }
};
struct MonitorOwner {
    net::DesyncMonitor monitor;
    MonitorOwner(net::MessageTransport& transport, net::DesyncOptions options) : monitor(transport, options) {}
};

class GcRoom {
public:
    GcRoom(std::vector<std::shared_ptr<Local>> locals, const net::GalacticLobbyOptions& options, std::uint32_t host_row, std::uint32_t capacity = 0)
        : network(static_cast<std::uint32_t>(locals.size())) {
        const auto count = static_cast<unsigned>(locals.size());
        for (unsigned peer = 0; peer < count; ++peer) {
            peers.push_back({locals[peer], std::make_unique<viewer::GalacticLobbyBackend>(locals[peer]->content), 0, 0});
            peers[peer].backend->set_adapter_factory([this, peer](const net::SetupOffer&, const cp::MpAgreement& agreement)
                -> eawr::core::Result<std::unique_ptr<net::SessionAdapter>> {
                ++peers[peer].builds;
                return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::make_unique<TickZeroAdapter>(agreement.session));
            });
            transports.push_back(std::make_unique<eawr::testing::FakeTransport>(network, peer));
        }
        const auto game = require(net::encode_galactic_lobby_game(options));
        net::LobbySelection host_choice;
        const auto row = std::find_if(options.rows.begin(), options.rows.end(), [&](const auto& value) { return value.player == host_row; });
        std::uint32_t side{};
        for (const auto& value : options.rows) if (value.human) { ++side; if (value.player == host_row) break; }
        host_choice = {"host", row->faction, 0, side, host_row};
        sessions.push_back(require(net::LobbySession::host(*transports[0], "gc-room", capacity ? capacity : std::min(count, net::max_lobby_seats),
            host_choice, host_row, game, hooks(0))));
        for (unsigned peer = 1; peer < count; ++peer) sessions.push_back(require(net::LobbySession::join(*transports[peer], "gc-room", 0, hooks(peer))));
        rounds(3);
    }
    net::LobbySession& host() { return sessions[0]; }
    // A kicked seat is out of the room: it takes no further part in readiness or the barrier.
    void drop_last() { sessions.pop_back(); }
    std::uint64_t revision() { return host().snapshot().revision; }
    void admit(unsigned peer, gc::PlayerId row, const std::string& name) {
        const auto options = require(net::decode_galactic_lobby_game(host().snapshot().game));
        const auto seat = seat_of(peer, 0, options, row, name);
        require(host().admit(peer, "identity-" + std::to_string(peer), seat.selection, row, revision()));
        rounds(3);
    }
    void round(std::uint64_t delta = 1) {
        network.round += delta;
        for (auto& session : sessions) require(session.pump(network.round));
    }
    void rounds(unsigned count) { for (unsigned i = 0; i < count; ++i) round(); }
    void all_ready() {
        round(2000);
        for (auto& session : sessions) require(session.request(net::LobbyAction::ready, revision()));
        rounds(3);
        for (const auto& seat : host().snapshot().seats) expect(seat.ready, "every seat readies the same revision");
    }
    void propose() { all_ready(); require(host().start(revision())); rounds(3); }
    void commit() {
        propose();
        for (unsigned peer = 1; peer < sessions.size(); ++peer) require(sessions[peer].accept_setup());
        for (unsigned i = 0; i < 400 && !std::all_of(sessions.begin(), sessions.end(), [](const auto& s) { return s.snapshot().phase == net::LobbyPhase::committed; }); ++i) round();
        expect(std::all_of(sessions.begin(), sessions.end(), [](const auto& s) { return s.snapshot().phase == net::LobbyPhase::committed; }),
            "every peer reaches the committed room");
        for (unsigned i = 0; i < 200 && !std::all_of(sessions.begin(), sessions.end(), [](const auto& s) { return s.result_ready(); }); ++i) round();
    }
    net::LobbyHooks hooks(unsigned peer) {
        net::LobbyHooks value;
        value.validate = [this, peer](const net::LobbySnapshot& snapshot, bool starting) { return peers[peer].backend->validate(snapshot, starting); };
        value.offer = [this, peer](const net::LobbySnapshot& snapshot, const std::vector<net::SetupSeat>& seats) {
            return peers[peer].backend->offer(snapshot, seats);
        };
        value.setup = [this, peer](net::MessageTransport& transport, const net::LobbySnapshot&) -> eawr::core::Result<net::LobbySetupContext> {
            ++peers[peer].contexts;
            net::DesyncOptions monitor_options; monitor_options.exchange_every = 1000;
            auto owner = std::make_shared<MonitorOwner>(transport, monitor_options);
            auto supplied = providers(owner->monitor, executor);
            supplied.identify = [this, peer](const net::SetupOffer& selected) { return peers[peer].backend->identify(selected); };
            supplied.build = [this, peer](const net::SetupOffer& selected) { return peers[peer].backend->build(selected); };
            net::LobbySetupContext context; context.providers = std::move(supplied); context.lifetime = std::move(owner);
            return eawr::core::Result<net::LobbySetupContext>::success(std::move(context));
        };
        value.remove_peer = [](auto, auto) {};
        return value;
    }
    eawr::testing::FakeNetwork network;
    sim::InlineExecutor executor;
    std::vector<PeerState> peers;
    std::vector<std::unique_ptr<eawr::testing::FakeTransport>> transports;
    std::vector<net::LobbySession> sessions;
};

std::vector<std::shared_ptr<Local>> locals_of(std::size_t count) {
    std::vector<std::shared_ptr<Local>> out;
    for (std::size_t i = 0; i < count; ++i) out.push_back(std::make_shared<Local>());
    return out;
}
std::string hex(const Digest& digest) {
    std::string out;
    for (const auto byte : digest) { out.push_back("0123456789abcdef"[byte >> 4U]); out.push_back("0123456789abcdef"[byte & 15U]); }
    return out;
}

void check_committed(GcRoom& room, const std::string& label, std::size_t humans, std::uint64_t seed = match_seed) {
    std::vector<net::LobbyResult> results;
    for (auto& session : room.sessions) results.push_back(require(session.take_result()));
    const auto& first = results.front().setup().offer();
    expect(first.domain == net::galactic_setup_domain && first.seats.size() == humans, label + ": the offer is the galactic domain with a peer per human");
    const auto payload = require(net::decode_galactic_setup(first.setup));
    for (std::size_t peer = 0; peer < results.size(); ++peer) {
        auto& setup = results[peer].setup();
        expect(setup.adapter().completed_tick() == 0, label + ": the barrier never steps tick one");
        expect(setup.adapter().state_hash() == hex(payload.roster.tick_zero), label + ": every peer's own tick-zero digest equals the agreed one");
        expect(setup.offer().seats == first.seats && setup.offer().seed == first.seed && setup.offer().seed == seed, label + ": seats and seed agreed");
        expect(room.peers[peer].builds == 1, label + ": each peer built from its own derivation once");
        expect(room.peers[peer].agreement()->content == room.peers[0].agreement()->content, label + ": identical derived identities");
    }
    for (std::size_t index = 0; index < payload.roster.seats.size(); ++index) {
        const auto& seat = payload.roster.seats[index];
        if (seat.kind != sim::SeatKind::human) continue;
        expect(first.seats[index].seat == seat.seat_id && first.seats[index].player == seat.player, label + ": a peer per roster human seat");
    }
}

void two_peer_room() {
    auto locals = locals_of(2);
    const auto options = two_side_options(*locals[0]);
    GcRoom room(locals, options, 1);
    room.admit(1, 2, "guest");
    expect(net::lobby_game_mode(room.host().snapshot().game) == net::LobbyGameMode::galactic
        && net::lobby_game_mode(room.sessions[1].snapshot().game) == net::LobbyGameMode::galactic, "host and guest agree on the mode");
    expect(room.host().snapshot() == room.sessions[1].snapshot() && room.host().snapshot().seats.size() == 2, "both peers hold one snapshot");
    expect(!room.host().start(room.revision()), "Start needs every seat ready");
    // A host edit of the GC options reaches the guest and clears readiness.
    room.all_ready();
    auto changed = options; changed.custom = true; changed.credits = 7000; changed.starting_tech = 2; changed.maximum_tech = 4;
    require(room.host().set_game(require(net::encode_galactic_lobby_game(changed)), room.revision())); room.rounds(3);
    expect(room.sessions[1].snapshot().game == room.host().snapshot().game && !room.host().snapshot().seats[1].ready
        && require(net::decode_galactic_lobby_game(room.sessions[1].snapshot().game)).credits == 7000, "GC option edits are one shared revision and clear readiness");
    auto bad = options; bad.rows[2].human = true;
    expect(!room.host().set_game(require(net::encode_galactic_lobby_game(bad)), room.revision()), "the host's validator refuses an unplayable human row");
    auto non_mp = require(net::encode_galactic_lobby_game(options)); non_mp.map = "Solo";
    expect(!room.host().set_game(non_mp, room.revision()), "the host's validator refuses a campaign outside the multiplayer list");
    // A skirmish option blob in a GC room's place is refused by the GC validator.
    expect(!room.host().set_game(net::LobbyGame{"Multiplayer", {'1', ' ', '3'}}, room.revision()), "skirmish bytes are not GC options");
    room.commit();
    check_committed(room, "two peers", 2);
}

// The host switches a playable row Open <-> AI; a joined peer sees each edit and cannot be left on an AI row.
void host_toggles_row_open_ai() {
    auto locals = locals_of(2);
    const auto options = two_side_options(*locals[0]);
    GcRoom room(locals, options, 1);
    room.admit(1, 1, "guest");
    const auto toggled = [&](bool human) {
        auto edited = require(net::decode_galactic_lobby_game(room.host().snapshot().game));
        edited.rows[1].human = human; // EMPIRE
        return edited;
    };
    require(room.host().set_game(require(net::encode_galactic_lobby_game(toggled(false))), room.revision())); room.rounds(3);
    expect(!require(net::decode_galactic_lobby_game(room.sessions[1].snapshot().game)).rows[1].human, "the guest sees the Empire row switched to AI");
    require(room.host().set_game(require(net::encode_galactic_lobby_game(toggled(true))), room.revision())); room.rounds(3);
    expect(require(net::decode_galactic_lobby_game(room.sessions[1].snapshot().game)).rows[1].human, "AI -> Open frees the row again");
    // A guest holding the row blocks the switch to AI.
    auto other = locals_of(2);
    GcRoom held(other, two_side_options(*other[0]), 1);
    held.admit(1, 2, "guest");
    expect(!held.host().set_game(require(net::encode_galactic_lobby_game([&] {
        auto edited = require(net::decode_galactic_lobby_game(held.host().snapshot().game)); edited.rows[1].human = false; return edited; }())),
        held.revision()), "a row a guest holds cannot become AI");
    // Both humans on Rebel, Empire host-toggled to AI, on the highest seed the AI perception accepts: the peers agree on the AI setup.
    auto fresh = locals_of(2);
    auto start = two_side_options(*fresh[0]);
    start.seed = std::numeric_limits<std::uint32_t>::max();
    GcRoom ai_room(fresh, start, 1);
    ai_room.admit(1, 1, "guest");
    auto ai_empire = require(net::decode_galactic_lobby_game(ai_room.host().snapshot().game));
    ai_empire.rows[1].human = false;
    require(ai_room.host().set_game(require(net::encode_galactic_lobby_game(ai_empire)), ai_room.revision())); ai_room.rounds(3);
    ai_room.all_ready();
    ai_room.commit();
    check_committed(ai_room, "highest 32-bit seed", 2, std::numeric_limits<std::uint32_t>::max());
}

void nine_seat_room() {
    auto locals = locals_of(9);
    const auto options = coop_options(*locals[0]);
    GcRoom room(locals, options, 1);
    for (unsigned peer = 1; peer < 9; ++peer) room.admit(peer, 1, "p" + std::to_string(peer));
    expect(room.host().snapshot().seats.size() == 9 && room.sessions[8].snapshot() == room.host().snapshot(), "a nine-seat GC room stages");
    // Ready and Start run the offer's own rules: nine humans cannot start (at most eight human seats).
    room.round(2000);
    expect(!room.host().request(net::LobbyAction::ready, room.revision()), "nine humans are refused at Ready");
    const auto refusal = net::validate_galactic_lobby(room.host().snapshot(), true, locals[0]->catalog());
    expect(!refusal && refusal.error().message.find("at most 8 human seats") != std::string::npos, "the refusal names the human seat limit");
    expect(room.host().snapshot().phase == net::LobbyPhase::staging, "a refused Ready leaves the room staging");
    require(room.host().kick(8, room.revision())); room.rounds(4);
    expect(room.host().snapshot().seats.size() == 8, "the host removes the ninth seat");
    room.drop_last();
    room.commit();
    std::vector<net::LobbyResult> results;
    for (auto& session : room.sessions) results.push_back(require(session.take_result()));
    const auto payload = require(net::decode_galactic_setup(results[0].setup().offer().setup));
    expect(results.size() == 8 && payload.roster.teams.size() == 1 && payload.roster.seats.size() == 10 && results[0].setup().offer().seats.size() == 8,
        "eight humans on one side against two AI factions");
    for (unsigned peer = 0; peer < 8; ++peer) {
        expect(results[peer].setup().adapter().state_hash() == hex(payload.roster.tick_zero), "peer " + std::to_string(peer) + " reached the agreed tick zero");
        expect(room.peers[peer].agreement()->content == room.peers[0].agreement()->content, "peer " + std::to_string(peer) + " derived the same identities");
    }
}

void two_sides_with_extra_ai() {
    // Eight seats on two human sides (up to the human limit), one more faction left to the AI.
    auto locals = locals_of(8);
    const auto options = two_side_options(*locals[0]);
    GcRoom room(locals, options, 1);
    for (unsigned peer = 1; peer < 8; ++peer) room.admit(peer, peer % 2 == 0 ? 1 : 2, "p" + std::to_string(peer));
    room.commit();
    std::vector<net::LobbyResult> results;
    for (auto& session : room.sessions) results.push_back(require(session.take_result()));
    const auto payload = require(net::decode_galactic_setup(results[0].setup().offer().setup));
    expect(payload.roster.teams.size() == 2 && results[0].setup().offer().seats.size() == 8, "two human sides of four");
    for (std::size_t peer = 0; peer < results.size(); ++peer)
        expect(results[peer].setup().adapter().state_hash() == hex(payload.roster.tick_zero), "every seat reached the agreed tick zero");
}

void content_mismatch_returns_to_staging() {
    auto locals = locals_of(2);
    locals[1]->content.xml_digest = std::string(64, 'd');
    const auto options = two_side_options(*locals[0]);
    GcRoom room(locals, options, 1);
    room.admit(1, 2, "guest");
    room.propose();
    (void)room.sessions[1].accept_setup();
    room.rounds(30);
    expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.peers[1].builds == 0,
        "a peer whose installed AI data differs is refused before any shared step and the room returns to staging");
}

// ---- the viewer's own match (what the lobby screen drives) over real loopback TCP ----

void reoffer_keeps_agreed_services() {
    auto local = std::make_shared<Local>();
    using Services = eawr::platform::CampaignServiceConfiguration;
    std::vector<std::pair<std::shared_ptr<cp::GameSession>, std::shared_ptr<Services>>> prepared;
    local->content.prepare_session = [&](const cp::CampaignSetupPlan& plan, std::string_view)
        -> eawr::core::Result<cp::MpPreparedSession> {
        auto session = std::make_shared<cp::GameSession>(require(cp::GameSession::create(plan.galaxy())));
        auto services = std::make_shared<Services>();
        prepared.emplace_back(session, services);
        return eawr::core::Result<cp::MpPreparedSession>::success({session, services});
    };
    viewer::GalacticLobbyBackend backend(local->content);
    auto options = two_side_options(*local);
    auto snapshot = snapshot_of(options, {seat_of(0, 1, options, 1, "Host"), seat_of(1, 2, options, 2, "Guest")});
    const std::vector<net::SetupSeat> seats{{0, {1}, 1}, {1, {2}, 2}};
    const auto offer_a = require(backend.offer(snapshot, seats));
    auto changed = options; changed.seed += 1;
    snapshot.game = require(net::encode_galactic_lobby_game(changed));
    const auto offer_b = require(backend.offer(snapshot, seats));
    require(backend.identify(offer_a));
    const auto agreed_a = backend.agreement();
    const auto services_a = prepared.back().second;
    expect(agreed_a->session == prepared.back().first && agreed_a->runtime_services == services_a,
        "agree A retains A's session and matching runtime services after a re-offer");
    require(backend.offer(snapshot, seats));
    expect(backend.agreement() == agreed_a && backend.runtime_services() == services_a,
        "a later re-offer cannot replace the agreed root's services");
    require(backend.identify(offer_b));
    expect(backend.agreement()->session != agreed_a->session && backend.runtime_services() != services_a,
        "a new agreement switches owner and services together");
    expect(agreed_a->runtime_services == services_a, "a retained launch still owns A's services");
}

// The same projection used by ViewerHost, compiled without Godot.
struct LaunchOptions {
    std::shared_ptr<viewer::GalacticLanMatch> lan_match;
    std::shared_ptr<viewer::GalacticLaunch> lan_start;
    std::vector<eawr::viewer::galactic::LanPlayer> lan_players;
    std::string campaign;
    std::uint32_t seat{};
};
void verify_launch(const std::vector<std::shared_ptr<viewer::GalacticLaunch>>& launches) {
    using Live = eawr::platform::LiveGameSession;
    std::vector<std::unique_ptr<Live>> games;
    for (std::size_t peer = 0; peer < launches.size(); ++peer) {
        const auto& launch = launches[peer];
        LaunchOptions options;
        eawr::viewer::galactic::apply_lobby_launch(options, launch);
        expect(options.lan_match && options.lan_start == launch && options.campaign == "Multiplayer"
            && options.seat == launch->seat, "viewer options retain the agreed campaign, local seat and match");
        expect(options.lan_players.size() == launches.size(), "one LAN HUD identity per human seat");
        for (std::size_t i = 0; i < options.lan_players.size(); ++i) {
            const auto& player = options.lan_players[i];
            const auto& seat = launch->roster.seats[i];
            expect(player.peer == i && player.seat == seat.seat && player.player == seat.selection.name
                && player.faction == seat.selection.faction, "LAN HUD identities follow the dense agreed seat order");
        }
        expect(options.lan_match->local_seat().value == launch->seat, "driver and viewer use the same local seat");
        expect(launch->owner->session && launch->owner->session->completed_tick() == 0,
            "launch keeps the derived tick-zero session");
        Live::Options live; live.pacing = Live::Pacing::driven; live.workers = peer % 2 == 0 ? 1 : 4;
        games.push_back(require(launch->start(std::move(live))));
    }
    constexpr std::uint64_t ticks = 12;
    for (auto& game : games) game->advance_to(ticks);
    for (auto& game : games) expect(game->wait_for(ticks, std::chrono::seconds(20)), "committed GC lobby advances agreed ticks");
    for (auto& game : games) game->stop();
    const auto hashes = games.front()->tick_hashes();
    expect(hashes.size() >= ticks, "all agreed tick hashes were recorded");
    for (const auto& game : games) expect(game->tick_hashes() == hashes, "lobby-launched peers have equal state hashes on every tick");
}

void viewer_staging_room() {
    Local host_content, guest_content;
    auto host_backend = std::make_shared<viewer::GalacticLobbyBackend>(host_content.content);
    auto guest_backend = std::make_shared<viewer::GalacticLobbyBackend>(guest_content.content);
    const viewer::LanLobbyBuilder builder = [](const auto&) {
        eawr::core::Diagnostic failure; failure.code = "EAWR-TEST-0002"; failure.message = "a campaign room never builds a skirmish setup";
        return eawr::core::Result<eawr::sim::tactical::TacticalSetup>::failure(std::move(failure));
    };
    auto host = viewer::make_lan_match(), guest = viewer::make_lan_match();
    host->configure_seats({1, 2}); guest->configure_seats({1, 2});
    host->configure_lobby("Host", 0, {}, builder); guest->configure_lobby("Guest", 1, {}, builder);
    host->configure_galactic(host_backend); guest->configure_galactic(guest_backend);

    // The mode is the host's: a room that never gets the options is a skirmish room.
    {
        auto plain = viewer::make_lan_match();
        plain->configure_seats({1, 2}); plain->configure_lobby("Plain", 0, {}, builder); plain->configure_galactic(host_backend);
        plain->host(0);
        expect(!plain->galactic_room() && !plain->take_galactic_launch() && !plain->set_galactic_game(net::GalacticLobbyOptions{}), "no GC options, no GC room (and empty options are refused)");
        plain->cancel();
    }
    host->host(0);
    const auto options = two_side_options(host_content);
    expect(host->set_galactic_game(options), "the host opens the room in Galactic Conquest mode");
    expect(host->galactic_room(), "the room's mode is read from the host's options");
    const eawr::skirmish::FixtureOptions none{};
    host->pump_setup(none);
    guest->join("127.0.0.1:" + std::to_string(host->view().port));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::string milestone = "admission";
    const auto wait = [&](const auto& predicate) {
        while (true) {
            host->pump_setup(none);
            guest->pump_setup(none);
            if (predicate()) return;
            if (std::chrono::steady_clock::now() >= deadline) {
                const auto describe = [](const auto& peer) {
                    const auto state = peer->lobby_state();
                    const auto start = peer->galactic_start();
                    return "room=" + std::to_string(state ? static_cast<int>(state->snapshot.phase) : -1)
                        + " setup=" + std::to_string(start && start->room->setup_phase()
                            ? static_cast<int>(*start->room->setup_phase()) : -1) + " " + peer->view().message;
                };
                expect(false, "galactic staging deadline at " + milestone + "; host " + describe(host) + "; guest " + describe(guest));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    const auto synchronized = [&] {
        const auto host_state = host->lobby_state();
        const auto guest_state = guest->lobby_state();
        return host_state && guest_state && host_state->snapshot.seats.size() == 2 && guest_state->snapshot == host_state->snapshot
            && !guest_state->identity_pending && !host_state->identity_pending;
    };
    wait(synchronized);
    expect(guest->galactic_room() && net::lobby_game_mode(guest->lobby_state()->snapshot.game) == net::LobbyGameMode::galactic,
        "the guest reads the same mode from the same revision");
    {
        const auto state = host->lobby_state();
        const auto& seats = state->snapshot.seats;
        expect(seats[0].selection.slot == 1 && seats[0].selection.faction == "REBEL" && seats[0].selection.team == 1
            && seats[1].selection.slot == 2 && seats[1].selection.faction == "EMPIRE" && seats[1].selection.team == 2
            && seats[0].player == 1 && seats[1].player == 2, "the host sits on the first human row, the guest on the free one");
    }
    // A host edit of the options is one shared revision.
    auto changed = options;
    changed.custom = true; changed.credits = 6500; changed.starting_tech = 2; changed.maximum_tech = 4;
    expect(host->lobby_galactic_game(changed), "the host edits the campaign options");
    milestone = "option edit";
    wait([&] { return synchronized() && require(net::decode_galactic_lobby_game(guest->lobby_state()->snapshot.game)).credits == 6500; });
    expect(!guest->lobby_galactic_game(options) && !guest->set_galactic_game(options), "a guest cannot edit the options or the mode");
    {   // Not a multiplayer campaign: refused by the host's validator, the room unchanged.
        const auto before = host->lobby_state()->snapshot;
        auto solo = changed; solo.campaign = "Solo";
        expect(!host->lobby_galactic_game(solo) && host->lobby_state()->snapshot == before, "a campaign outside the multiplayer list is refused");
    }
    // A seat moves to another faction row; its side follows the row.
    auto choice = guest->lobby_state()->snapshot.seats[1].selection;
    choice.slot = 1;
    expect(guest->lobby_select(choice), "the guest picks the host's faction row");
    milestone = "seat move";
    wait([&] { return synchronized() && host->lobby_state()->snapshot.seats[1].selection.slot == 1 && host->lobby_state()->snapshot.seats[1].player == 1; });
    expect(host->lobby_state()->snapshot.seats[1].selection.faction == "REBEL" && host->lobby_state()->snapshot.seats[1].selection.team == 1
        && host->lobby_state()->snapshot.seats[1].player == 1, "the seat's faction, side and owner follow the row (co-op on one faction)");
    choice = guest->lobby_state()->snapshot.seats[1].selection; choice.slot = 2;
    expect(guest->lobby_select(choice), "the guest moves back to the second faction");
    wait([&] { return synchronized() && host->lobby_state()->snapshot.seats[1].selection.slot == 2 && host->lobby_state()->snapshot.seats[1].player == 2; });

    expect(host->lobby_ready(true) && guest->lobby_ready(true), "both players ready the campaign setup");
    milestone = "ready";
    wait([&] { return host->lobby_state()->snapshot.seats[0].ready && host->lobby_state()->snapshot.seats[1].ready; });
    expect(host_backend->can_start() && !host->galactic_start(), "production adapter is installed, but no launch precedes Start");
    milestone = "Start enabled";
    wait([&] { return host->lobby_state()->start_enabled; });
    host->start(none);
    milestone = "Start frozen";
    wait([&] { return host->galactic_start() && guest->galactic_start() && guest->lobby_state()->snapshot.phase == net::LobbyPhase::starting; });
    const auto host_start = *host->galactic_start(), guest_start = *guest->galactic_start();
    expect(host_start.options == guest_start.options && host_start.options.credits == 6500 && host_start.room && guest_start.room,
        "both peers hold the frozen room and the agreed options at the seam");
    expect(host_start.room->snapshot().phase == net::LobbyPhase::starting && guest_start.room->snapshot().frozen_revision != 0
        && guest_start.room->snapshot().frozen_revision == host_start.room->snapshot().frozen_revision,
        "the frozen revision is the same on both peers");
    std::shared_ptr<viewer::GalacticLaunch> host_launch, guest_launch;
    milestone = "committed launch";
    wait([&] {
        if (!host_launch) host_launch = host->take_galactic_launch();
        if (!guest_launch) guest_launch = guest->take_galactic_launch();
        return host_launch && guest_launch;
    });
    expect(!host->take_galactic_launch() && !guest->take_galactic_launch(), "a committed launch is taken exactly once");
    verify_launch({host_launch, guest_launch});
    host_launch.reset(); guest_launch.reset();
    host->cancel(); guest->cancel();
}

void viewer_many_seat_launch(std::size_t count) {
    std::vector<std::shared_ptr<Local>> locals;
    std::vector<std::shared_ptr<viewer::LanMatch>> peers;
    std::vector<gc::PlayerId> owners;
    for (unsigned i = 1; i <= count; ++i) owners.push_back(i);
    for (std::size_t i = 0; i < count; ++i) {
        locals.push_back(std::make_shared<Local>());
        auto match = viewer::make_lan_match();
        match->configure_seats(owners);
        match->configure_lobby("Player " + std::to_string(i + 1), 0, {}, [](const auto&) {
            eawr::core::Diagnostic error; error.code = "EAWR-TEST-0002"; error.message = "GC never builds tactical setup";
            return eawr::core::Result<eawr::sim::tactical::TacticalSetup>::failure(error);
        });
        match->configure_galactic(std::make_shared<viewer::GalacticLobbyBackend>(locals.back()->content));
        peers.push_back(std::move(match));
    }
    peers[0]->host(0);
    expect(peers[0]->set_galactic_game(two_side_options(*locals[0])), "open nine-seat campaign room");
    const eawr::skirmish::FixtureOptions none{};
    peers[0]->pump_setup(none);
    for (std::size_t i = 1; i < count; ++i) peers[i]->join("127.0.0.1:" + std::to_string(peers[0]->view().port));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    const auto wait = [&](const auto& predicate) {
        while (true) {
            for (const auto& peer : peers) peer->pump_setup(none);
            if (predicate()) return;
            for (const auto& peer : peers) expect(peer->view().phase != viewer::LanPhase::ended,
                "nine-seat campaign connection refused: " + peer->view().message);
            expect(std::chrono::steady_clock::now() < deadline, "nine-seat GC launch deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    wait([&] {
        for (const auto& peer : peers) {
            const auto state = peer->lobby_state();
            if (!state || state->snapshot.seats.size() != count || state->identity_pending) return false;
        }
        return true;
    });
    if (count == 9) {
        // The room can stage nine peers, but the agreed GC roster permits at most eight humans.
        for (const auto& peer : peers) {
            expect(!peer->lobby_ready(true), "a ninth human is refused before Start");
            expect(peer->lobby_state()->ready_reason.find("at most 8 human seats") != std::string::npos,
                "the nine-human refusal names the existing roster limit");
            expect(!peer->take_galactic_launch(), "a refused nine-human room cannot launch a solo or mismatched campaign");
        }
        // Display identities have no authority to bypass that roster rule; qualify the nine-row projection independently.
        eawr::testing::FakeNetwork network(static_cast<std::uint32_t>(count));
        const auto snapshot = peers[0]->lobby_state()->snapshot;
        for (std::size_t i = 0; i < count; ++i) {
            eawr::testing::FakeTransport transport(network, static_cast<net::PeerIndex>(i));
            viewer::GalacticLanOptions roster;
            for (const auto& seat : snapshot.seats) roster.seats.push_back({seat.seat, true});
            auto launch = std::make_shared<viewer::GalacticLaunch>();
            launch->options = two_side_options(*locals[0]); launch->roster = snapshot;
            launch->seat = snapshot.seats[i].seat.value;
            launch->match = std::make_shared<viewer::GalacticLanMatch>(transport, std::move(roster));
            LaunchOptions projected;
            eawr::viewer::galactic::apply_lobby_launch(projected, launch);
            expect(projected.lan_match && projected.lan_match->local_seat().value == projected.seat
                && projected.lan_players.size() == count, "nine-row projection retains the match and local seat");
            for (std::size_t row = 0; row < count; ++row)
                expect(projected.lan_players[row].peer == row && projected.lan_players[row].seat == snapshot.seats[row].seat
                    && projected.lan_players[row].player == snapshot.seats[row].selection.name
                    && projected.lan_players[row].faction == snapshot.seats[row].selection.faction,
                    "nine-row projection follows every agreed display identity");
        }
        for (const auto& peer : peers) peer->cancel();
        return;
    }
    for (const auto& peer : peers) expect(peer->lobby_ready(true), "multi-seat ready");
    wait([&] { return peers[0]->lobby_state()->start_enabled; });
    peers[0]->start(none);
    std::vector<std::shared_ptr<viewer::GalacticLaunch>> launches(count);
    wait([&] {
        bool all = true;
        for (std::size_t i = 0; i < count; ++i) {
            if (!launches[i]) launches[i] = peers[i]->take_galactic_launch();
            all = all && static_cast<bool>(launches[i]);
        }
        return all;
    });
    verify_launch(launches);
    launches.clear();
    for (const auto& peer : peers) peer->cancel();
}

void installed_stock_native_ai_agreement(const cp::MpLocalContent& local,
    const gc::MpLobbyCampaign& campaign, std::uint64_t seed) {
    // The real AI perception setup must accept every seed the lobby can hand out. The default rows are the owner's
    // layout: two human sides (Rebel host, Empire guest) and the remaining stock faction (Underworld) as AI.
    auto options = require(net::default_galactic_lobby_options(campaign, difficulty, seed));
    expect(options.seed == seed, "the lobby options carry the host's seed");
    options.custom = true; options.credits = 10000; options.allow_heroes = false;
    options.starting_tech = 2; options.maximum_tech = 5;
    std::vector<gc::PlayerId> humans;
    for (const auto& row : options.rows) if (row.human) humans.push_back(row.player);
    auto snapshot = snapshot_of(options, {seat_of(0, 1, options, humans.at(0), "host"),
        seat_of(1, 2, options, humans.at(1), "guest")});
    for (auto& seat : snapshot.seats) seat.ready = true;
    viewer::GalacticLobbyBackend backend(local);
    const auto offered = backend.offer(snapshot, {{0, {1}, humans.at(0)}, {1, {2}, humans.at(1)}});
    if (!offered) throw std::runtime_error(campaign.name + ": " + eawr::core::format_diagnostic(offered.error()));
    const auto& offer = offered.value();
    const auto identity = require(backend.identify(offer));
    const auto first = backend.agreement();
    expect(first && first->session && first->session->native_ai_installed(),
        "stock MP campaign prepares native AI through the real lobby loader: " + campaign.name);
    std::vector<std::string> baseline;
    std::vector<std::shared_ptr<const gc::GalacticSnapshot>> baseline_views;
    // Two independent peers per stock map, reusing the host's actual preparation.
    // The authored observer case below retains the full 1/2/4/8-by-view matrix.
    for (const auto workers : {1U, 8U}) {
        const std::size_t peer_index = workers == 1 ? 0 : 1;
        viewer::GalacticLobbyBackend peer(local);
        auto agreement = first;
        if (workers != 1) {
            expect(require(peer.identify(offer)).content == identity.content, "stock peers independently agree all content domains");
            agreement = peer.agreement();
        }
        expect(agreement && agreement->tick_zero == first->tick_zero, "stock peers agree tick-zero digest");
        auto& session = *agreement->session;
        expect(session.native_ai_installed(), "each stock peer installs native AI before tick zero");
        const auto native = require(session.save_galactic_ai());
        expect(native.state && native.state->components.contains("galactic-ai-lifecycle"),
            "stock preparation includes the selected native AI lifecycle in canonical state");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes{session.state_sha256()};
        const auto check_view = [&](std::size_t tick) {
            const auto player = humans.at(peer_index);
            const auto view = session.galaxy().view_snapshot({player});
            expect(view && view->view_player() == player, "stock peer publishes its own human view");
            expect(!view->observations().empty(), "stock human view contains observer rows");
            for (const auto& row : view->observations())
                expect(row.player == player, "stock view filters foreign private observer rows");
            for (const auto& row : view->production_offers())
                expect(row.player == player, "stock view filters foreign private production offers");
            if (const auto* forecast = view->economy_forecast())
                expect(forecast->player == player, "stock view filters foreign economy forecast");
            if (peer_index == 0) baseline_views.push_back(view);
            else {
                const auto& other = *baseline_views.at(tick);
                const auto same_rows = [](const auto a, const auto b) {
                    return std::equal(a.begin(), a.end(), b.begin(), b.end());
                };
                expect(view->completed_tick() == other.completed_tick() && view->clock() == other.clock()
                    && same_rows(view->players(), other.players()) && same_rows(view->planets(), other.planets())
                    && same_rows(view->routes(), other.routes()) && same_rows(view->fleets(), other.fleets()),
                    "stock peer views agree shared state at every tick while private rows remain observer-local");
            }
        };
        check_view(0);
        for (unsigned tick = 0; tick < 12; ++tick) {
            hashes.push_back(require(session.step(executor)).state_hash.get());
            check_view(tick + 1);
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "stock campaign agrees every tick across both peers at 1 and 8 workers");
        std::cout << "stock native AI agreement campaign=" << campaign.name << " workers=" << workers
            << " peer=" << peer_index << " ticks=12 tick_zero=" << hashes.front() << " digest=" << hashes.back() << '\n';
    }
}

void installed_observer_agreement(bool stock_only = false) {
    const auto root = eawr::testing::game_root();
    if (!root) {
        std::cout << (stock_only ? "SKIPPED: stock multiplayer AI installed data not requested\n"
                                : "installed observer campaign not requested\n");
        return;
    }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(require(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
    const auto files = std::make_shared<const eawr::vfs::Vfs>(require(eawr::vfs::Vfs::mount(mounts)));
    const auto catalog = require(eawr::data::load_catalog(*files, eawr::data::Profile::foc));
    auto loaded = require(viewer::load_galactic_lobby_content(*files, catalog.catalog, files));
    // The real viewer loader owns the fiscal planner, stock placement, conflict services and native AI.
    loaded.local.data = loaded.data.get(); loaded.local.types = loaded.types.get();
    auto campaigns = cp::list_mp_lobby_campaigns(*loaded.data);
    std::sort(campaigns.begin(), campaigns.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    expect(!campaigns.empty(), "installed data contains a multiplayer campaign");
    if (stock_only) {
        const std::array stock_names{
            "Multiplayer_Sandbox_Clusters", "Multiplayer_Sandbox_Equal_Footing", "Multiplayer_Sandbox_Gateways",
            "Multiplayer_Sandbox_Heart_Of_The_Maw", "Multiplayer_Sandbox_Origin", "Multiplayer_Sandbox_Rim_Worlds"};
        expect(campaigns.size() == stock_names.size(), "all six stock MP campaigns exercise native AI agreement");
        for (const auto* name : stock_names) {
            const auto* campaign = cp::find_mp_lobby_campaign(campaigns, name);
            expect(campaign != nullptr, "stock native AI campaign exists: " + std::string(name));
            if (!campaign) continue;
            // The seed a host really draws, and (once) the widest one the lobby accepts.
            const auto drawn = net::draw_galactic_session_seed();
            expect(drawn <= std::numeric_limits<std::uint32_t>::max(), "a drawn host seed fits in 32 bits");
            installed_stock_native_ai_agreement(loaded.local, *campaign, drawn);
            if (name == stock_names.front()) installed_stock_native_ai_agreement(loaded.local, *campaign, std::numeric_limits<std::uint32_t>::max());
        }
        return;
    }

    // Author a three-home campaign using installed identifiers, leaving all production
    // loaders, placement, services, AI and lobby agreement callbacks untouched.
    // Stock MP native AI and inert starting protection have their own batch-only CTest entry.
    const auto* source = loaded.data->campaign("Sandbox_Equal_Footing_Empire");
    if (!source) throw std::runtime_error("installed fixture identifier source missing");
    struct FixtureDirectory {
        std::filesystem::path root;
        ~FixtureDirectory() { eawr::testing::remove_temp_directory(root); }
    } fixture{std::filesystem::temp_directory_path() / ("eawr-observer-campaign-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
    std::filesystem::create_directories(fixture.root / "XML");
    const auto xml_name = [](std::string_view name) {
        if (name.empty() || name.find_first_of("<>&\"'") != std::string_view::npos)
            throw std::runtime_error("fixture identifier needs XML escaping");
        return std::string(name);
    };
    std::ostringstream authored;
    authored << "<Campaigns><Campaign Name=\"Observer_Agreement_Test\">"
        "<Is_Listed>Yes</Is_Listed><Is_Multiplayer>Yes</Is_Multiplayer>"
        "<Supports_Custom_Settings>Yes</Supports_Custom_Settings>"
        "<Min_Custom_Tech_Level>1</Min_Custom_Tech_Level><Max_Custom_Tech_Level>5</Max_Custom_Tech_Level>"
        "<Is_Story_Campaign>No</Is_Story_Campaign><Planet_Auto_Reveal>No</Planet_Auto_Reveal>"
        "<Starting_Active_Player>Empire</Starting_Active_Player>";
    std::set<std::string> homes;
    for (const auto* faction : {"Empire", "Rebel", "Underworld"}) {
        const auto force = std::find_if(source->starting_forces.begin(), source->starting_forces.end(), [&](const auto& row) {
            const auto* type = loaded.types->find(row.type);
            return row.faction == faction && type && type->object_class == "SpaceUnit"
                && type->can_hyperspace_without_ability.value_or(true) && !type->named_hero.value_or(false);
        });
        if (force == source->starting_forces.end()) throw std::runtime_error("fixture needs an installed ordinary ship");
        const auto home = std::find_if(source->home_locations.begin(), source->home_locations.end(),
            [&](const auto& row) { return row.faction == faction; });
        const auto planet = home == source->home_locations.end() ? force->location : home->name;
        expect(homes.insert(planet).second, "fixture factions have distinct secret home planets");
        authored << "<Home_Location>" << faction << ',' << xml_name(planet) << "</Home_Location>"
            << "<Starting_Forces>" << faction << ',' << xml_name(planet) << ',' << xml_name(force->type) << "</Starting_Forces>"
            << "<Starting_Credits>" << faction << ",10000</Starting_Credits>"
            << "<Starting_Tech_Level>" << faction << ",2</Starting_Tech_Level>"
            << "<Max_Tech_Level>" << faction << ",5</Max_Tech_Level>";
        // The solo observer reference needs explicit campaign pairs. Derive these
        // from faction data; MP selection itself does not consume those pairs.
        const auto* definition = loaded.data->faction(faction);
        if (!definition || definition->basic_ai.empty()) throw std::runtime_error("fixture needs an authored faction Basic_AI");
        authored << "<AI_Player_Control>" << faction << ',' << xml_name(definition->basic_ai) << "</AI_Player_Control>";
    }
    authored << "<Locations>";
    for (const auto& home : homes) authored << xml_name(home) << ",";
    authored << "</Locations></Campaign></Campaigns>";
    const auto write = [&](const char* name, const std::string& text) {
        std::ofstream output(fixture.root / "XML" / name); output << text;
        if (!output) throw std::runtime_error("cannot write authored observer campaign");
    };
    write("CampaignFiles.xml", "<Campaign_Files><File>Observer_Test_Campaign.xml</File></Campaign_Files>");
    write("Observer_Test_Campaign.xml", authored.str());
    mounts.insert(mounts.begin(), eawr::vfs::MountSpec{"observer-test", fixture.root, "data", {}});
    const auto fixture_files = std::make_shared<const eawr::vfs::Vfs>(require(eawr::vfs::Vfs::mount(mounts)));
    const auto fixture_catalog = require(eawr::data::load_catalog(*fixture_files, eawr::data::Profile::foc));
    loaded = require(viewer::load_galactic_lobby_content(*fixture_files, fixture_catalog.catalog, fixture_files));
    loaded.local.data = loaded.data.get(); loaded.local.types = loaded.types.get();
    campaigns = cp::list_mp_lobby_campaigns(*loaded.data);
    expect(campaigns.size() == 1 && campaigns.front().name == "Observer_Agreement_Test", "real XML loader selects the authored fixture");
    const auto& selected = campaigns.front();
    auto options = require(net::default_galactic_lobby_options(selected, difficulty, match_seed));
    options.custom = true; options.credits = 10000; options.allow_heroes = false;
    options.starting_tech = 2; options.maximum_tech = 5;
    const auto human = std::find_if(options.rows.begin(), options.rows.end(), [](const auto& row) { return row.human; });
    cp::CampaignSetupOptions solo; solo.forced_seed = match_seed; solo.solo_reference_human = human->player;
    solo.custom = cp::CampaignCustomOptions{options.credits, options.starting_tech, options.maximum_tech, false};
    for (const auto& faction : selected.factions) {
        solo.players.push_back({{faction.player, faction.faction_id, faction.flags}, faction.name, true});
        if (!faction.neutral) solo.seats.push_back({{faction.player}, faction.player,
            faction.player == *solo.solo_reference_human ? gc::SeatKind::human : gc::SeatKind::ai,
            faction.player, true, faction.player == *solo.solo_reference_human ? faction.player : 0});
    }
    std::vector<std::string> type_names;
    for (const auto& type : loaded.types->types) type_names.push_back(type.name);
    std::sort(type_names.begin(), type_names.end());
    for (const auto& name : type_names) solo.types.push_back({solo.types.size() + 1, name});
    const auto solo_prepared = require(eawr::platform::PreparedLiveCampaign::create(fixture_files,
        fixture_catalog.catalog, *loaded.data, *loaded.types, selected.name, std::move(solo), difficulty, true));
    expect(solo_prepared->native_ai_installed(), "same authored campaign prepares the real solo native AI path");
    std::cout << "authored observer campaign=" << options.campaign << " allow_heroes=false solo_prepare=passed\n";
    std::vector<gc::PlayerId> humans;
    for (const auto& row : options.rows) if (row.human) humans.push_back(row.player);
    expect(humans.size() == 2, "installed campaign offers two different human factions");
    auto snapshot = snapshot_of(options, {seat_of(0, 1, options, humans.at(0), "host"),
        seat_of(1, 2, options, humans.at(1), "guest")});
    for (auto& seat : snapshot.seats) seat.ready = true;
    viewer::GalacticLobbyBackend host(loaded.local);
    const auto offer = require(host.offer(snapshot, {{0, {1}, humans.at(0)}, {1, {2}, humans.at(1)}}));
    const auto identity = require(host.identify(offer));
    const auto first = host.agreement();
    expect(first && first->session->native_ai_installed() && first->runtime_services,
        "real prepared lobby path installs native AI before tick zero");
    expect(std::any_of(first->plan->galaxy().seats.begin(), first->plan->galaxy().seats.end(),
        [](const auto& row) { return row.active && row.kind == gc::SeatKind::ai; }),
        "installed observer campaign retains an active AI faction alongside the two humans");
    expect(first->plan->human_observers().size() == 2 && !first->plan->solo_reference_human(),
        "multiplayer plan has canonical human contexts and no solo reference");
    std::vector<std::string> baseline;
    std::vector<std::uint8_t> replay;
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const auto seat : {1U, 2U}) {
        viewer::GalacticLobbyBackend peer(loaded.local);
        expect(require(peer.identify(offer)).content == identity.content, "prepared peers agree all content domains");
        const auto agreement = peer.agreement();
        expect(agreement->tick_zero == first->tick_zero, "prepared peers agree tick-zero digest");
        auto& session = *agreement->session;
        const auto player = humans.at(seat - 1);
        const auto view = session.galaxy().view_snapshot({player});
        expect(view && view->view_player() == player, "peer resolves its own agreed human context");
        for (const auto& row : view->observations()) expect(row.player == player, "prepared view excludes foreign observer rows");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes{session.state_sha256()};
        for (unsigned tick = 0; tick < 12; ++tick) {
            (void)session.galaxy().view_snapshot({player});
            hashes.push_back(require(session.step(executor)).state_hash.get());
        }
        const auto bytes = require(gc::write_replay(session.record()));
        if (baseline.empty()) { baseline = hashes; replay = bytes; }
        else expect(hashes == baseline && bytes == replay,
            "real prepared lobby campaign agrees each tick and replay on both views and 1/2/4/8 workers");
        std::cout << "prepared observer agreement campaign=" << options.campaign << " workers=" << workers << " seat=" << seat
            << " ticks=12 digest=" << hashes.back() << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--stock-ai") {
            installed_observer_agreement(true);
            viewer::shutdown_lan_drains();
            std::cout << "stock galactic lobby AI contracts passed\n";
            return 0;
        }
        if (argc != 1) throw std::runtime_error("usage: net_galactic_lobby_tests [--stock-ai]");
        codec_and_mode();
        skirmish_bytes_pinned();
        campaign_list();
        staging_validation();
        roster_draft();
        two_peer_room();
        host_toggles_row_open_ai();
        nine_seat_room();
        two_sides_with_extra_ai();
        content_mismatch_returns_to_staging();
        reoffer_keeps_agreed_services();
        viewer_staging_room();
        viewer_many_seat_launch(9);
        viewer_many_seat_launch(8);
        installed_observer_agreement();
        viewer::shutdown_lan_drains();
        std::cout << "galactic lobby contracts passed: gc1 codec and versioning, campaign list, staging refusals, two-peer and nine-seat rooms, skirmish bytes pinned\n";
        return 0;
    } catch (const std::exception& error) {
        viewer::shutdown_lan_drains();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
