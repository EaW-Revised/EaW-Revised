// GCMP-07 (#4363): the lobby's Galactic Conquest mode over the real lobby core and the #3768 setup offer. The
// options codec and its versioning, the campaign list, the staging refusals, and two-peer and nine-seat rooms that
// reach the tick-zero barrier on identities each peer derived from its own content. The skirmish options bytes stay
// pinned: a GC mode changes nothing about a skirmish room.
#include "setup_test_support.hpp"
#include "eawr/vfs/vfs.hpp"
#include "fake_transport.hpp"

#include "galactic_lobby_backend.hpp"
#include "match_transport.hpp"
#include "galactic_lan_match.hpp"
#include "lan_match.hpp"
#include "galactic_launch_options.hpp"
#include "../../apps/viewer/src/galactic/faction_choices.hpp"
#include "eawr/presentation/ui/campaign_setup.hpp"
#include "../../apps/viewer/src/checkpoint_lobby_model.hpp"
#include "eawr/platform/agreed_campaign.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "eawr/campaign/mp_agreement.hpp"
#include "eawr/campaign/mp_lobby_catalog.hpp"
#include "eawr/net/galactic_lobby.hpp"
#include "eawr/net/galactic_adapter.hpp"
#include "eawr/checkpoint/game.hpp"
#include "../../src/platform/galactic/live_save_transport.hpp"
#include "eawr/net/lobby_session.hpp"
#include "eawr/sim/tactical/victory.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/mods/save_description.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/saved_games.hpp"
#include "eawr/platform/live_game_lockstep.hpp"
#include "../support/env.hpp"
#include "../support/temp_dir.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <memory>
#include <future>
#include <condition_variable>
#include <deque>
#include <functional>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <variant>

namespace {
using namespace setup_test;
using eawr::core::Result;
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace dg = eawr::data::galactic;
namespace viewer = eawr::presentation::godot_backend;

void checkpoint_buffered_departure() {
    using namespace std::chrono_literals;
    net::TcpOptions options; options.build_identity = "checkpoint-buffered-departure";
    auto listening = require(net::TcpTransport::host("127.0.0.1", 0, options));
    auto connected = require(net::TcpTransport::connect("127.0.0.1", listening->port(), options));
    auto* raw_guest = connected.get();
    viewer::MatchTransport host(std::move(listening)), guest(std::move(connected));
    auto host_worker = std::make_shared<net::GalacticCheckpointWorker>();
    auto guest_worker = std::make_shared<net::GalacticCheckpointWorker>();
    host.start_checkpoint(host_worker); guest.start_checkpoint(guest_worker);
    const auto wait = [&](const auto& ready) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!ready()) {
            expect(std::chrono::steady_clock::now() < deadline, "buffered departure reaches its native boundary");
            std::this_thread::sleep_for(1ms);
        }
    };
    wait([&] { return host.link(1) == net::LinkState::open && guest.link(0) == net::LinkState::open; });
    const std::vector<std::uint8_t> ordinary{17, 42, 23};
    require(host.send(1, ordinary));
    wait([&] { return guest_worker->pending_from(0); });
    require(host.close_peer(1));
    wait([&] {
        const std::lock_guard lock(net::checkpoint_io_mutex());
        return raw_guest->link(0) == net::LinkState::closed;
    });
    expect(guest.link(0) == net::LinkState::open,
        "queued foreground traffic remains readable before publishing native departure");
    const auto last = guest.poll();
    expect(last && last->from == 0 && last->frame == ordinary,
        "the last foreground frame survives native peer departure exactly once");
    expect(guest.link(0) == net::LinkState::closed && !guest.poll(),
        "departure becomes visible immediately after the foreground queue drains");
    host.stop_checkpoint(); guest.stop_checkpoint();
}

void checkpoint_failure_stays_visible() {
    using namespace std::chrono_literals;
    net::TcpOptions options; options.build_identity = "checkpoint-sticky-failure";
    viewer::MatchTransport host(require(net::TcpTransport::host("127.0.0.1", 0, options)));
    auto guest = require(net::TcpTransport::connect("127.0.0.1", host.port(), options));
    const auto connected = std::chrono::steady_clock::now() + 5s;
    while (host.link(1) != net::LinkState::open || guest->link(0) != net::LinkState::open) {
        static_cast<void>(host.poll()); static_cast<void>(guest->poll());
        expect(std::chrono::steady_clock::now() < connected, "sticky failure pair connects");
        std::this_thread::sleep_for(1ms);
    }
    auto worker = std::make_shared<net::GalacticCheckpointWorker>();
    {
        const std::lock_guard lock(net::checkpoint_io_mutex());
        host.start_checkpoint(worker);
        // Native polling is blocked by this gate. At most one bounded command batch can drain,
        // so bounded repeated enqueueing deterministically reaches the fatal queue limit.
        for (unsigned attempt = 0; attempt < 256 && !worker->failure(); ++attempt)
            worker->cancel(host, "queue_limit_test");
        expect(worker->failure().has_value(), "command overflow publishes a fatal worker error");
    }
    host.stop_checkpoint();
    host.start_checkpoint(worker); // the lobby makes this automatic call before checking failure()
    expect(!worker->running() && worker->failure().has_value() && host.diagnostic().has_value(),
        "automatic staging attachment preserves a stopped worker failure for the lobby");
    const std::vector<std::uint8_t> checkpoint{net::save_manifest_kind, 0};
    const std::vector<std::uint8_t> ordinary{17, 23, 42};
    require(guest->send(0, checkpoint)); require(guest->send(0, ordinary));
    const auto delivered = std::chrono::steady_clock::now() + 5s;
    std::optional<net::Received> frame;
    while (!(frame = host.poll())) {
        static_cast<void>(guest->poll());
        expect(std::chrono::steady_clock::now() < delivered, "ordinary traffic drains after fatal checkpoint failure");
        std::this_thread::sleep_for(1ms);
    }
    expect(frame->frame == ordinary && !host.poll() && host.diagnostic().has_value(),
        "fatal checkpoint frames never escape to the lobby and ordinary frames arrive exactly once");
}
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
    expect(text == "gc2 3 4242 2 15 0 0 0 0 1 \"Normal_Default\" 4 1 1 0 \"REBEL\" 1 2 2 2 0 \"EMPIRE\" 1 2 3 3 0 \"UNDERWORLD\" 0 2 4 4 0 \"NEUTRAL\" 0 2",
        "gc2 options bytes: " + text);
    expect(net::lobby_game_mode(game) == net::LobbyGameMode::galactic, "the tag makes it a Galactic Conquest room");
    expect(require(net::decode_galactic_lobby_game(game)) == options, "options round trip");
    expect(require(net::encode_galactic_lobby_game(require(net::decode_galactic_lobby_game(game)))) == game, "re-encoding keeps the bytes");

    auto custom = options;
    custom.custom = true; custom.credits = 12000; custom.starting_tech = 2; custom.maximum_tech = 4; custom.allow_heroes = false;
    custom.auto_resolve = 3; custom.rows[1].dropout = gc::FactionControl::passive; custom.seed = 0xFFFFFFFFULL;
    custom.vote_timeout = 45;
    expect(require(net::decode_galactic_lobby_game(require(net::encode_galactic_lobby_game(custom)))) == custom, "custom options round trip");
    {   // #6156: the battle-vote timeout is a host setting. It defaults to the retail 15 s, follows the campaign's own constant
        // when the data states one (clamped to the host range 5..120), and rides the agreed bytes.
        expect(options.vote_timeout == net::galactic_lobby_default_vote_timeout && net::galactic_lobby_default_vote_timeout == 15,
            "a new room offers the retail 15 s");
        expect(net::galactic_lobby_clamp_vote_timeout(-3) == 5 && net::galactic_lobby_clamp_vote_timeout(0) == 5
            && net::galactic_lobby_clamp_vote_timeout(4) == 5 && net::galactic_lobby_clamp_vote_timeout(5) == 5
            && net::galactic_lobby_clamp_vote_timeout(60) == 60 && net::galactic_lobby_clamp_vote_timeout(120) == 120
            && net::galactic_lobby_clamp_vote_timeout(121) == 120 && net::galactic_lobby_clamp_vote_timeout(100000) == 120,
            "the host range is 5 to 120 seconds");
        const auto offered = [&](std::optional<std::int64_t> authored) {
            auto campaign = local.catalog().front(); campaign.vote_timeout_seconds = authored;
            return require(net::default_galactic_lobby_options(campaign, difficulty, match_seed)).vote_timeout;
        };
        expect(offered(std::nullopt) == 15 && offered(30) == 30 && offered(2) == 5 && offered(900) == 120 && offered(0) == 5,
            "the campaign's own constant is the room's default, clamped to the host range");
        auto codec = options; codec.vote_timeout = 120;
        const auto bytes = require(net::encode_galactic_lobby_game(codec));
        expect(std::string(bytes.options.begin(), bytes.options.end()).find(" 2 120 0 ") != std::string::npos,
            "the timeout is in the agreed bytes after the auto-resolve value");
        expect(require(net::decode_galactic_lobby_game(bytes)).vote_timeout == 120, "the timeout round trips");
        codec.vote_timeout = gc::mp_roster_max_vote_timeout_seconds + 1U;
        expect(!net::encode_galactic_lobby_game(codec), "a timeout beyond the campaign's lifetime is refused");
    }

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
    future.options[2] = '3';
    const auto refused = net::decode_galactic_lobby_game(future);
    expect(!refused && refused.error().message.find("version 3") != std::string::npos, "an unknown version is refused by name");
    auto previous = game;
    previous.options[2] = '1';
    const auto old_refused = net::decode_galactic_lobby_game(previous);
    expect(!old_refused && old_refused.error().message.find("version 1") != std::string::npos,
        "the previous version, which has no vote timeout, is refused by name");
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
    m.win_float = Fixed::from_raw(99); m.win_condition = "WC"; m.tactical_win_condition = "SW";
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

// #5512: the campaign content loads on a worker. While it runs the UI thread's other backend calls return at once
// instead of waiting behind the load, and a failed load still ends in a ready, refusing backend.
void content_load_never_blocks_the_ui_thread() {
    // The gate is shared with the detached prefetch thread, which may outlive a failing expect below.
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool released = false, entered = false;
        std::atomic<int> loads{0};
        void release() { { const std::lock_guard lock(mutex); released = true; } changed.notify_all(); }
    };
    const auto gate = std::make_shared<Gate>();
    auto backend = std::make_shared<viewer::GalacticLobbyBackend>(viewer::GalacticLobbyLoader([gate] {
        ++gate->loads;
        std::unique_lock lock(gate->mutex);
        gate->entered = true;
        gate->changed.notify_all();
        gate->changed.wait(lock, [&] { return gate->released; });
        return eawr::core::Result<viewer::GalacticLobbyContent>::failure(
            eawr::core::Diagnostic{.code = "EAWR-TEST-5512", .message = "synthetic slow load",
                .logical_path = std::nullopt, .line = std::nullopt, .column = std::nullopt, .source_id = std::nullopt});
    }));
    // A failing expect must still let the gated loader return (the frame task and the prefetch thread wait on it),
    // or the test hangs instead of failing; `frame` is declared outside the try so its wait happens after the release.
    std::future<bool> frame;
    try {
        expect(!backend->content_ready(), "a backend built from a loader is not ready before the load");
        viewer::GalacticLobbyBackend::prefetch_content(backend);
        viewer::GalacticLobbyBackend::prefetch_content(backend);
        {
            std::unique_lock lock(gate->mutex);
            expect(gate->changed.wait_for(lock, std::chrono::seconds(30), [&] { return gate->entered; }), "the prefetch thread runs the loader");
        }
        // The calls the lobby makes every frame, on a stand-in UI thread with a bounded deadline.
        frame = std::async(std::launch::async, [&] {
            const auto view = backend->checkpoint_view();
            backend->clear_checkpoint();
            return backend->agreement() == nullptr && !view && !backend->content_ready();
        });
        expect(frame.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
            "agreement, checkpoint view and clear return while the content load is still running");
        if (frame.wait_for(std::chrono::seconds(0)) == std::future_status::ready) expect(frame.get(), "the frame calls saw an unloaded, empty backend");
        gate->release();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!backend->content_ready() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        expect(backend->content_ready(), "the backend becomes ready when the load returns");
        const auto error = backend->load_error();
        expect(error && error->code == "EAWR-TEST-5512", "a failed load is kept as the load error");
        expect(backend->campaigns().empty(), "a failed load lists no campaigns");
        expect(gate->loads == 1, "two prefetch requests run the loader once");
    } catch (...) {
        gate->release();
        throw;
    }
}

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

// ---- the campaign list shows the authored name and description (#4553) ----

void campaign_labels() {
    Local local;
    local.data.campaigns[0].text_id = "TEXT_CAMPAIGN_SINGLE_21";
    local.data.campaigns[0].description_text = "TEXT_CAMPAIGN_DESC_21";
    const auto list = local.catalog();
    expect(list[0].text_id == "TEXT_CAMPAIGN_SINGLE_21" && list[0].description_text == "TEXT_CAMPAIGN_DESC_21",
        "the list carries the campaign's Text_ID and Description_Text keys");
    expect(list[0].planet_points.size() == 1 && list[0].planet_points[0].first.raw() == 0 && list[0].planet_points[0].second.raw() == 0,
        "the list carries each planet's galactic position for the staging preview");
    const std::map<std::string, std::string, std::less<>> table{
        {"TEXT_CAMPAIGN_SINGLE_21", "Galactic Conquest"}, {"TEXT_CAMPAIGN_DESC_21", "Rule the galaxy.\\nSecond line."}};
    const auto resolve = [&](const std::string_view key) -> std::optional<std::string> {
        const auto found = table.find(key);
        if (found == table.end()) return std::nullopt;
        return found->second;
    };
    auto labels = cp::label_mp_lobby_campaign(list[0], resolve);
    expect(labels.title == "Galactic Conquest" && labels.description == "Rule the galaxy.\nSecond line." && labels.missing.empty(),
        "the stock campaign shows its localized name and description, with the line break expanded");
    auto gone = list[0];
    gone.text_id = "TEXT_MOD_NAME"; gone.description_text = "TEXT_MOD_DESC";
    labels = cp::label_mp_lobby_campaign(gone, resolve);
    expect(labels.title == gone.name && labels.description.empty()
            && labels.missing == std::vector<std::string>{"TEXT_MOD_NAME", "TEXT_MOD_DESC"},
        "a campaign whose text is missing falls back to its ID, no description, and names the missing keys");
    gone.text_id.clear(); gone.description_text.clear();
    labels = cp::label_mp_lobby_campaign(gone, resolve);
    expect(labels.title == gone.name && labels.description.empty() && labels.missing.empty(),
        "a campaign that authors no text shows its ID without a log line");
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

// A mod with no Is_Multiplayer campaign (#6010): its dummy entries start as a faction nobody can hold. Names are arbitrary.
void coop_catalog_without_multiplayer_campaign() {
    Local local(Names{"ALPHA_HOUSE", "BETA_GUILD", "GAMMA_SWARM", "OMEGA_NOBODY"});
    auto& data = local.data;
    // a faction the data lists as not playable, and one that is neutral
    dg::Faction hostile; hostile.name = "DELTA_RAIDERS"; hostile.is_neutral = false; hostile.is_playable = false;
    hostile.displayed_tech_level_adjustment = 0;
    data.factions.push_back(hostile);
    auto base = data.campaigns.front();
    base.is_multiplayer = false; base.name = "Filler_One"; base.starting_active_player = "Delta_Raiders";
    base.home_locations.push_back({"DELTA_RAIDERS", "P"});
    data.campaigns.front() = base;
    auto add = [&](const std::string& name, const std::string& starter) {
        auto row = base; row.name = name; row.starting_active_player = starter; data.campaigns.push_back(row);
    };
    add("Filler_Neutral", "OMEGA_NOBODY");     // neutral starter
    add("Filler_Missing", "NOT_IN_THE_DATA");  // starter is no faction
    add("Filler_AiOnly", "GAMMA_SWARM");       // a faction the data does not let a player hold
    expect(cp::list_all_lobby_campaigns(data).empty(), "entries whose starter cannot be held are not listed");
    expect(cp::first_hostable_lobby_campaign(cp::list_all_lobby_campaigns(data)) == nullptr, "an empty catalog has no hostable row");
    add("Real_Solo_Beta", "Beta_Guild");
    add("Real_Solo_Alpha", "ALPHA_HOUSE");
    const auto list = cp::list_all_lobby_campaigns(data);
    expect(list.size() == 2 && list[0].name == "Real_Solo_Beta" && list[1].name == "Real_Solo_Alpha",
        "playable starters are listed in registry order, whatever their names");
    const auto* first = cp::first_hostable_lobby_campaign(list);
    expect(first == &list[0], "the room opens on the first eligible row, not on the registry's first entry");
    auto options = net::default_galactic_lobby_options(*first, difficulty, match_seed);
    expect(options.has_value(), "the first eligible row opens a room");
    // the unplayable-first order still hosts: a hand-built catalog with a row that cannot seat anyone ahead of a good one
    auto catalog = list;
    catalog.insert(catalog.begin(), catalog[0]);
    for (auto& faction : catalog[0].factions) faction.playable = false;
    expect(cp::first_hostable_lobby_campaign(catalog) == &catalog[1], "a row with no playable faction is skipped");
    expect(!net::default_galactic_lobby_options(catalog[0], difficulty, match_seed).has_value(), "the skipped row is the one that would be refused");
    // co-op rows name the faction the players take
    expect(cp::lobby_campaign_row_title(list[0], "Gateways") == "BETA_GUILD - Gateways"
        && cp::lobby_campaign_row_title(list[1], "Gateways") == "ALPHA_HOUSE - Gateways",
        "a co-op row names its faction first, so a cut caption keeps it");
    auto multiplayer = list[0]; multiplayer.single_player_rules = false;
    expect(cp::lobby_campaign_row_title(multiplayer, "Gateways") == "Gateways", "a multiplayer row keeps its title");
}

// Design B2: single-player entries join the list only on request, as co-op rows on their authored human faction.
void coop_single_player_sets() {
    Local local;
    auto& data = local.data;
    const auto base = data.campaigns.front();
    auto add = [&](const std::string& name, auto change) { auto row = base; row.name = name; change(row); data.campaigns.push_back(row); };
    add("SoloRebel", [](dg::Campaign& c) { c.is_multiplayer = false; c.starting_active_player = "Rebel"; });
    add("SoloHidden", [](dg::Campaign& c) { c.is_multiplayer = false; c.starting_active_player = "Rebel"; c.is_listed = false; });
    add("SoloTutorial", [](dg::Campaign& c) { c.is_multiplayer = false; c.starting_active_player = "Rebel"; c.tutorial = true; });
    add("SoloStory", [](dg::Campaign& c) { c.is_multiplayer = false; c.starting_active_player = "Rebel"; c.is_story_campaign = true; });
    add("SoloNoHuman", [](dg::Campaign& c) { c.is_multiplayer = false; c.starting_active_player.clear(); });
    expect(cp::list_mp_lobby_campaigns(data).size() == 1, "single-player entries stay out of the multiplayer list");
    const auto coop_tab = cp::list_coop_sp_lobby_campaigns(data);
    expect(coop_tab.size() == 1 && coop_tab[0].name == "SoloRebel" && coop_tab[0].single_player_rules,
        "the co-op list holds only listed, non-tutorial, non-story entries with a human faction");
    const auto list = cp::list_all_lobby_campaigns(data);
    expect(list.size() == 2 && list[0].name == "Multiplayer" && !list[0].single_player_rules && list[1] == coop_tab[0],
        "the combined list is the multiplayer tab then the co-op tab");
    expect(list[0] == cp::list_mp_lobby_campaigns(data)[0], "the multiplayer row is unchanged by the co-op list");
    std::size_t playable{};
    for (const auto& faction : list[1].factions) if (faction.playable) { ++playable; expect(faction.name == "REBEL", "only the authored human faction is playable"); }
    expect(playable == 1, "one playable faction on a single-player entry");
    auto options = require(net::default_galactic_lobby_options(list[1], difficulty, match_seed, 3, true));
    std::size_t humans{};
    for (const auto& row : options.rows) humans += row.human ? 1 : 0;
    expect(humans == 1, "a single-player entry plans one co-op side even when two sides are asked for");
    const auto snapshot = snapshot_of(options, {seat_of(0, 1, options, 1, "host"), seat_of(1, 2, options, 1, "guest")});
    auto solo_game = snapshot; solo_game.game.map = "SoloRebel";
    expect(net::validate_galactic_lobby(solo_game, true, list).has_value(), "co-op seats validate on a single-player entry");
    expect(refused(solo_game, false, local.catalog(), "not an eligible campaign"), "the multiplayer catalog still refuses it");
    viewer::GalacticLobbyBackend backend(local.content);
    expect(backend.campaigns() == list && backend.validate(solo_game, true).has_value(),
        "the production backend offers and validates the co-op campaign at Start");
    auto two_rows = options; two_rows.rows[1].human = true;
    expect(refused(snapshot_of(two_rows, {}), false, list, "cannot be held"), "a human row off the authored faction is refused");
}

// #6047: the difficulties a room offers are the entries the AI seats of the selected campaign can run. Start reads only the
// AI seats' player types, so a human starter's and a neutral faction's type never count, and an offered value is never refused.
void difficulty_follows_the_campaigns_ai_types() {
    using Levels = std::array<std::string, 3>;
    using Entries = std::array<std::string, 3>;
    // Two families (a major and a minor type), a type that maps one entry to two levels, and a starter-only family.
    const cp::LobbyAiLevels ai_levels = [](const std::string_view type) -> Levels {
        if (type == "MAJOR_AI" || type == "MAJOR2_AI") return {"Easy_Default", "Normal_Default", "Hard_Default"};
        if (type == "MINOR_AI") return {"Easy_Minor", "Normal_Minor", "Hard_Minor"};
        if (type == "SKEWED_AI") return {"Easy_Default", "Hard_Default", "Hard_Default"};
        if (type == "R_AI") return {"Easy_R", "Normal_R", "Hard_R"};
        if (type == "E_AI") return {"Easy_E", "Normal_E", "Hard_E"};
        return {};
    };
    const Entries default_family{"Easy_Default", "Normal_Default", "Hard_Default"};
    Local local;
    auto& data = local.data;
    for (const char* extra : {"EXTRA_ONE", "EXTRA_TWO"}) {
        auto faction = data.factions[2]; faction.name = extra; data.factions.push_back(faction);
    }
    const auto base = data.campaigns.front();
    // AI types go to the AI factions in turn: EMPIRE, UNDERWORLD, EXTRA_ONE, EXTRA_TWO (the starter is REBEL).
    const auto add = [&](const std::string& name, const std::vector<std::string>& types,
                         const std::vector<std::pair<std::string, std::string>>& more = {}) {
        auto row = base; row.name = name; row.is_multiplayer = false; row.starting_active_player = "REBEL";
        row.ai_player_control.clear();
        const char* const factions[] = {"EMPIRE", "UNDERWORLD", "EXTRA_ONE", "EXTRA_TWO"};
        for (std::size_t index = 0; index < types.size(); ++index) row.ai_player_control.push_back({factions[index], types[index]});
        for (const auto& [faction, type] : more) row.ai_player_control.push_back({faction, type});
        data.campaigns.push_back(row);
    };
    add("Majors", {"Major_Ai", "Major_Ai"});
    add("Mixed", {"Minor_Ai", "Major_Ai", "Major2_Ai"});
    add("MinorOnly", {"Minor_Ai"});
    add("Skewed", {"Major_Ai", "Skewed_Ai"});
    add("NoAi", {});
    // The reviewer's case: the human starter runs R_AI, the only AI seat runs E_AI; R_AI's entries cannot be run at Start.
    add("StarterAndAi", {"E_Ai"}, {{"REBEL", "R_Ai"}});
    // A type of the starter is the only carrier of an entry: the list must not offer it.
    add("StarterOnlyCarrier", {"Skewed_Ai"}, {{"REBEL", "Major_Ai"}});
    add("NeutralPair", {"Major_Ai"}, {{"NEUTRAL", "Minor_Ai"}});
    const auto list = cp::list_coop_sp_lobby_campaigns(data, ai_levels);
    const auto named = [&](const std::string_view name) -> const gc::MpLobbyCampaign& {
        const auto* found = cp::find_mp_lobby_campaign(list, name);
        expect(found != nullptr, "the co-op entry is listed");
        return *found;
    };
    expect(named("Majors").difficulties == default_family, "one AI family offers its own three entries");
    expect(named("Mixed").difficulties == default_family, "per level the entry most of the always-AI types share wins, though the minor type comes first");
    expect(named("MinorOnly").difficulties == Entries{"Easy_Minor", "Normal_Minor", "Hard_Minor"}, "a campaign of minor AI offers the minor entries");
    expect(named("Skewed").difficulties == Entries{"Easy_Default", "Normal_Default", ""},
        "an entry the AI types map to two levels is not offered: the AI setup would refuse it as ambiguous");
    expect(named("NoAi").difficulties == Entries{}, "a campaign without AI types states no list");
    expect(named("StarterAndAi").difficulties == Entries{"Easy_E", "Normal_E", "Hard_E"},
        "the human starter's AI type is not counted: its entries would be refused at Start");
    expect(named("StarterOnlyCarrier").difficulties == Entries{"Easy_Default", "", ""},
        "an entry only the starter's type carries is not offered, and the AI types' ambiguity still counts");
    expect(named("NeutralPair").difficulties == default_family, "a neutral faction's AI type is not counted");
    expect(cp::list_coop_sp_lobby_campaigns(data)[0].difficulties == Entries{}, "without the AI levels no campaign states a list");

    // Multiplayer entries: a non-playable faction is always AI; playable factions run their Basic_AI only when left to the AI.
    {
        Local mp;
        mp.data.factions[0].basic_ai = "MAJOR_AI"; mp.data.factions[1].basic_ai = "MINOR_AI";
        auto certain = mp.data;
        certain.campaigns[0].ai_player_control = {{"UNDERWORLD", "Major2_Ai"}};
        expect(cp::list_mp_lobby_campaigns(certain, ai_levels)[0].difficulties == default_family,
            "a multiplayer entry with an always-AI faction offers that type's entries, not the playable factions' Basic_AI");
        expect(cp::list_mp_lobby_campaigns(mp.data, ai_levels)[0].difficulties == Entries{},
            "playable factions that disagree share no entry that is safe whichever of them the AI plays");
        mp.data.factions[1].basic_ai = "MAJOR2_AI";
        expect(cp::list_mp_lobby_campaigns(mp.data, ai_levels)[0].difficulties == default_family,
            "playable factions' Basic_AI types that all carry an entry offer it");
    }

    // The default and the room options follow the list; every offered entry validates, anything else is refused with a reason.
    const auto& mixed = named("Mixed");
    expect(gc::lobby_default_difficulty(mixed, "Normal_Default") == "Normal_Default"
        && gc::lobby_default_difficulty(named("MinorOnly"), "Normal_Default") == "Normal_Minor"
        && gc::lobby_default_difficulty(named("NoAi"), "Normal_Default") == "Normal_Default", "a room starts at the campaign's Normal entry");
    gc::MpLobbyCampaign gap;
    gap.difficulties = {"Easy_X", "", "Hard_X"};
    expect(gc::lobby_default_difficulty(gap, "F") == "Hard_X", "without a Normal entry the middle of the valid ones, never silently the easiest");
    gap.difficulties = {"Easy_X", "", ""};
    expect(gc::lobby_default_difficulty(gap, "F") == "Easy_X", "a single valid entry is the default");
    expect(gc::lobby_difficulty_offered(mixed, "HARD_DEFAULT") && !gc::lobby_difficulty_offered(mixed, "Hard_Minor"),
        "the offered test folds ASCII case only");
    expect(require(net::default_galactic_lobby_options(named("MinorOnly"), "Normal_Default", match_seed)).difficulty == "Normal_Minor",
        "default options replace a difficulty the campaign's AI does not author");
    expect(require(net::default_galactic_lobby_options(mixed, "Hard_Default", match_seed)).difficulty == "Hard_Default",
        "default options keep an offered difficulty");
    for (const auto& entry : mixed.difficulties) {
        auto options = require(net::default_galactic_lobby_options(mixed, entry, match_seed, 3, false));
        expect(net::validate_galactic_lobby(snapshot_of(options, {seat_of(0, 1, options, 1, "host")}), false, list).has_value(),
            "every offered difficulty validates");
    }
    for (const char* wrong : {"Hard_Minor", "Normal_Skirmish", "Easy_Story"}) {
        auto options = require(net::default_galactic_lobby_options(mixed, "Normal_Default", match_seed, 3, false));
        options.difficulty = wrong;
        expect(refused(snapshot_of(options, {seat_of(0, 1, options, 1, "host")}), false, list, "does not apply to campaign Mixed"),
            "a difficulty the campaign does not offer is refused with its reason");
    }
    auto lower = require(net::default_galactic_lobby_options(mixed, "Normal_Default", match_seed, 3, false));
    lower.difficulty = "normal_default";
    expect(net::validate_galactic_lobby(snapshot_of(lower, {seat_of(0, 1, lower, 1, "host")}), false, list).has_value(),
        "the difficulty compares in any case, as the AI setup does");
    auto free_text = require(net::default_galactic_lobby_options(named("NoAi"), "Whatever_Mod", match_seed, 3, false));
    expect(free_text.difficulty == "Whatever_Mod"
        && net::validate_galactic_lobby(snapshot_of(free_text, {seat_of(0, 1, free_text, 1, "host")}), false, list).has_value(),
        "a campaign without a list keeps accepting an authored name");
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
    expect(refused(solo, false, catalog, "not an eligible campaign"), "a campaign outside the multiplayer list is refused");
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
    expect(draft.vote_timeout_seconds == 15, "the room's vote timeout is the roster's (#6156)");
    {
        auto edited = two; edited.vote_timeout = 90;
        const auto timed = require(net::galactic_roster_draft(snapshot_of(edited, {seat_of(0, 1, edited, 1, "host"), seat_of(1, 2, edited, 2, "guest")}), catalog));
        expect(timed.vote_timeout_seconds == 90, "a host's edited timeout reaches the agreed roster");
    }
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
    std::uint64_t tick_limit() const override { return sim::galactic::galactic_max_ticks; }
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
    struct Gate final : net::SessionMonitor {
        net::DesyncMonitor& monitor;
        bool advance{true};
        explicit Gate(net::DesyncMonitor& value) : monitor(value) {}
        bool needs_encoding(std::uint64_t tick, bool end) const override { return monitor.needs_encoding(tick, end); }
        eawr::core::Result<void> record(std::uint64_t tick, std::string_view hash, std::span<const std::uint8_t> state,
            std::span<const std::uint8_t> input, bool end) override { return monitor.record(tick, hash, state, input, end); }
        eawr::core::Result<void> receive(const net::Received& frame) override { return monitor.receive(frame); }
        eawr::core::Result<void> on_input_delay(std::uint64_t delay, std::uint64_t retained) override {
            return monitor.on_input_delay(delay, retained);
        }
        eawr::core::Result<void> on_departure(net::PeerIndex peer, std::uint64_t tick) override { return monitor.on_departure(peer, tick); }
        std::vector<net::PeerIndex> waiting_for_peers() const override { return monitor.waiting_for_peers(); }
        bool frozen() const override { return monitor.frozen(); }
        bool ready_for_resync() const override { return monitor.ready_for_resync(); }
        eawr::core::Result<void> on_resync(std::uint64_t tick, std::string_view hash) override { return monitor.on_resync(tick, hash); }
        eawr::core::Result<void> on_link_repair(net::PeerIndex peer, std::function<bool(net::PeerIndex)> ownership = {}) override {
            return monitor.on_link_repair(peer, std::move(ownership));
        }
        bool may_step() const override { return advance && monitor.may_step(); }
        eawr::core::Result<void> on_reconnect(std::uint64_t tick, std::string_view hash,
            std::span<const net::PeerIndex> inactive) override { return monitor.on_reconnect(tick, hash, inactive); }
    } gate;
    MonitorOwner(net::MessageTransport& transport, net::DesyncOptions options) : monitor(transport, options), gate(monitor) {}
};

class GcRoom {
public:
    GcRoom(std::vector<std::shared_ptr<Local>> locals, const net::GalacticLobbyOptions& options, std::uint32_t host_row, std::uint32_t capacity = 0, bool concurrent = false)
        : concurrent_prepare(concurrent), network(static_cast<std::uint32_t>(locals.size())) {
        const auto count = static_cast<unsigned>(locals.size());
        monitors.resize(count);
        for (unsigned peer = 0; peer < count; ++peer) {
            peers.push_back({locals[peer], std::make_unique<viewer::GalacticLobbyBackend>(locals[peer]->content), 0, 0});
            peers[peer].backend->set_adapter_factory([this, peer](const net::SetupOffer&, const cp::MpAgreement& agreement)
                -> eawr::core::Result<std::unique_ptr<net::SessionAdapter>> {
                ++peers[peer].builds;
                if (agreement.resume) {
                    const sim::PartitionExecutor& workers = resumed_workers ? static_cast<const sim::PartitionExecutor&>(*resumed_workers) : executor;
                    net::GalacticAdapterOptions options; options.clock_policy = resumed_clock_policy;
                    auto adapter = std::make_unique<net::GalacticSessionAdapter>(*agreement.session, workers, options);
                    if (!agreement.session->network_resume_state().empty()) {
                        auto restored = adapter->restore_network_state(agreement.session->network_resume_state());
                        if (!restored) return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::failure(restored.error());
                    }
                    return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::move(adapter));
                }
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
        if (concurrent_prepare) {
            value.preparation_inputs = [this, peer](const auto& snapshot, const auto& seats) {
                return peers[peer].backend->preparation_inputs(snapshot, seats);
            };
            value.preparation_domains = [this, peer](const auto& offer) {
                return peers[peer].backend->preparation_domains(offer);
            };
            value.preparation_mounts = [this, peer](const auto& snapshot, const auto* offer) {
                return peers[peer].backend->preparation_mounts(snapshot, offer);
            };
            value.discard_preparation = [this, peer] { peers[peer].backend->abandon_prepare(); };
        }
        value.bind_setup_seats = [this, peer](const auto& snapshot, const auto& seats) {
            return peers[peer].backend->bind_setup_seats(snapshot, seats);
        };
        value.application_frame = [this, peer](auto& transport, const auto& snapshot, const auto& frame, auto now) {
            if (!net::GalacticCheckpointTransfer::handles(frame.frame)) return false;
            peers[peer].backend->receive_transfer(transport, snapshot, frame, now); return true;
        };
        value.application_pump = [this, peer](auto& transport, const auto& snapshot, auto now) {
            peers[peer].backend->pump_transfer(transport, snapshot, now);
            const auto epoch = peers[peer].backend->selection_epoch();
            const bool changed = epoch != observed_epochs[peer]; observed_epochs[peer] = epoch; return changed;
        };
        value.validate = [this, peer](const net::LobbySnapshot& snapshot, bool starting) { return peers[peer].backend->validate(snapshot, starting); };
        value.follow_choice = [this, peer](const net::LobbySnapshot& before, net::LobbySnapshot& candidate, net::PeerIndex who) {
            if (peers[peer].backend->checkpoint_view()) return eawr::core::Result<void>::success();
            return net::follow_galactic_choice(before, candidate, who, peers[peer].backend->campaigns());
        };
        value.offer = [this, peer](const net::LobbySnapshot& snapshot, const std::vector<net::SetupSeat>& seats) {
            return peers[peer].backend->offer(snapshot, seats);
        };
        value.setup = [this, peer](net::MessageTransport& transport, const net::LobbySnapshot&) -> eawr::core::Result<net::LobbySetupContext> {
            ++peers[peer].contexts;
            net::DesyncOptions monitor_options; monitor_options.exchange_every = 1000;
            monitor_options.initial_tick = peers[peer].backend->initial_tick();
            auto owner = std::make_shared<MonitorOwner>(transport, monitor_options);
            monitors[peer] = owner;
            auto supplied = providers(owner->monitor, executor);
            supplied.monitor = &owner->gate;
            supplied.identify = [this, peer](const net::SetupOffer& selected) { return peers[peer].backend->identify(selected); };
            supplied.build = [this, peer](const net::SetupOffer& selected) { return peers[peer].backend->build(selected); };
            net::LobbySetupContext context; context.providers = std::move(supplied); context.lifetime = std::move(owner);
            context.options.session.initial_tick = peers[peer].backend->initial_tick();
            context.options.session.producer_sequences = peers[peer].backend->producer_sequences();
            context.options.session.final_tick_count = peers[peer].backend->final_tick_count();
            return eawr::core::Result<net::LobbySetupContext>::success(std::move(context));
        };
        value.remove_peer = [](auto, auto) {};
        return value;
    }
    bool concurrent_prepare;
    eawr::testing::FakeNetwork network;
    sim::InlineExecutor executor;
    std::shared_ptr<eawr::platform::ThreadWorkerAdapter> resumed_workers;
    net::GalacticClockPolicy resumed_clock_policy{net::GalacticClockPolicy::unrestricted};
    std::array<std::uint64_t, net::max_match_seats> observed_epochs{};
    std::vector<std::shared_ptr<MonitorOwner>> monitors;
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

// The live drivers poll concurrently, so their in-memory transport owns a lock.
struct LivePipes {
    std::mutex mutex;
    explicit LivePipes(std::size_t count) : inbox(count) {}
    std::vector<std::deque<net::Received>> inbox;
};

eawr::core::Result<eawr::sim::tactical::TacticalContent> pending_fixture_content(const cp::ConflictRecord& entry) {
    eawr::sim::tactical::TacticalContent content;
    if (entry.tactical.campaign) {
        content.economy.campaign = true;
        content.economy.players = entry.tactical.campaign->players;
    }
    return eawr::core::Result<eawr::sim::tactical::TacticalContent>::success(std::move(content));
}
class LivePipe final : public net::MessageTransport {
public:
    LivePipe(LivePipes& pipes, net::PeerIndex peer) : pipes_(pipes), peer_(peer) {}
    eawr::core::Result<void> send(net::PeerIndex to, std::span<const std::uint8_t> frame) override {
        const std::lock_guard lock(pipes_.mutex);
        pipes_.inbox.at(to).push_back({peer_, {frame.begin(), frame.end()}});
        return eawr::core::Result<void>::success();
    }
    std::optional<net::Received> poll() override {
        const std::lock_guard lock(pipes_.mutex);
        auto& queue = pipes_.inbox.at(peer_);
        if (queue.empty()) return {};
        auto value = std::move(queue.front()); queue.pop_front(); return value;
    }
    net::LinkState link(net::PeerIndex) const override { return net::LinkState::open; }
    net::PeerIndex self() const override { return peer_; }
    std::uint32_t peer_count() const override { return static_cast<std::uint32_t>(pipes_.inbox.size()); }
private:
    LivePipes& pipes_;
    net::PeerIndex peer_;
};

// Use the committed lobby core, the same retained file and the backend's
// private restore factory used by the live launch handover.
void recover_resumed_child(GcRoom& room, std::vector<net::LobbyResult> results,
    std::shared_ptr<const Bytes> bytes, unsigned workers) {
    namespace tactical = eawr::sim::tactical;
    using Refusal = net::GalacticRecoveryRefusal;
    std::array<std::shared_ptr<cp::GameSession>, 3> games;
    std::array<std::shared_ptr<net::LobbyResult>, 3> admissions;
    std::array<std::unique_ptr<viewer::GalacticLanMatch>, 3> matches;
    std::array<std::shared_ptr<eawr::platform::LiveGameStepDriver>, 3> drivers;
    std::array<unsigned, 3> publications{};
    std::vector<eawr::core::Diagnostic> rejected;
    auto refuse_restore = std::make_shared<std::atomic_bool>(false);
    for (unsigned peer = 0; peer < 3; ++peer) {
        games[peer] = room.peers[peer].agreement()->session;
        admissions[peer] = std::make_shared<net::LobbyResult>(std::move(results[peer]));
        const auto offer = admissions[peer]->setup().offer();
        viewer::GalacticLanOptions options;
        options.input_delay = offer.input_delay; options.final_tick_count = offer.final_tick_count;
        options.recovery_offer = offer; options.initial_checkpoint = bytes;
        options.recovery_restore = room.peers[peer].backend->recovery_factory(offer, workers, net::GalacticClockPolicy::limited);
        if (peer == 2) {
            auto restore = std::move(options.recovery_restore);
            options.recovery_restore = [restore = std::move(restore), refuse_restore](const auto& package)
                -> eawr::core::Result<std::unique_ptr<net::SessionAdapter>> {
                if (!refuse_restore->load()) return restore(package);
                eawr::core::Diagnostic error; error.code = "EAWR-TEST-RECOVERY"; error.message = "injected private restore failure";
                return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::failure(std::move(error));
            };
        }
        for (const auto& seat : offer.seats) options.seats.push_back({seat.seat, true});
        auto monitor = room.monitors[peer];
        matches[peer] = std::make_unique<viewer::GalacticLanMatch>(*room.transports[peer], std::move(options),
            admissions[peer], monitor->monitor, [monitor](bool advance) { monitor->gate.advance = advance; }, monitor);
        drivers[peer] = matches[peer]->driver();
    }
    const auto clock_start = std::chrono::steady_clock::now();
    const auto clock_base = room.network.round;
    const auto step = [&](unsigned peer, bool advance, std::span<const eawr::platform::LiveGameOrder> orders = {}) {
        eawr::platform::LiveGameStepContext context{*games[peer], *room.resumed_workers, advance, {},
            [&](auto error) { rejected.push_back(std::move(error)); }};
        context.adopt_recovered_owner = [&](eawr::platform::RecoveredGameOwner owner) {
            games[peer] = std::move(owner.session); ++publications[peer];
            return eawr::core::Result<void>::success();
        };
        ++room.network.round;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - clock_start).count();
        require(drivers[peer]->step(context, orders, clock_base + static_cast<std::uint64_t>(now)));
    };
    const auto run = [&](std::uint64_t target, bool include_returning = true) {
        const unsigned count = include_returning ? 3U : 2U;
        for (unsigned round = 0; round < 10000; ++round) {
            for (unsigned peer = 0; peer < count; ++peer) step(peer, games[peer]->completed_tick() < target);
            if (std::all_of(games.begin(), games.begin() + count,
                [&](const auto& game) { return game->completed_tick() == target; })) return;
        }
        throw std::runtime_error("resumed child fixture did not reach its target");
    };
    const auto initial = games[0]->completed_tick();
    const auto conflict = games[0]->pending_conflict()->id;
    const auto first = admissions[0]->setup().offer().seats[0].seat;
    const auto second = admissions[0]->setup().offer().seats[2].seat;
    const std::array<eawr::platform::LiveGameOrder, 1> choice{{{first, gc::BattleChoice{conflict, true}, {}}}};
    step(0, true, choice); run(initial + 12);
    expect(games[0]->battle() && games[1]->battle(), "saved pending conflict enters a child through the recorded choice");
    if (!games[0]->battle() || !games[1]->battle()) throw std::runtime_error("missing resumed child");
    const auto units = games[0]->battle()->units();
    const auto root = std::find_if(units.begin(), units.end(), [&](const auto& unit) {
        return unit.owner == 1 && unit.controller_seat == first;
    });
    if (root == units.end()) {
        std::string seen = "missing first-seat child unit " + std::to_string(first.value) + "; child units:";
        for (const auto& unit : units) seen += " [" + std::to_string(unit.entity_id) + " owner " + std::to_string(unit.owner)
            + " seat " + std::to_string(unit.controller_seat.value) + "]";
        throw std::runtime_error(seen);
    }
    const auto id = root->entity_id;
    const auto gift = [&](sim::SeatId from, sim::SeatId to) {
        tactical::PlayerCommand command{{0, 1, 1}, {id}, tactical::GiftPayload{to}};
        command.origin = tactical::CommandOrigin::seat; command.issuing_seat = from;
        const auto body = require(tactical::encode_command_body(command, games[0]->conflict()->tactical));
        return eawr::platform::LiveGameOrder{from,
            gc::TacticalLayerCommand{conflict, tactical::command_body_opcode(command), body}, {}};
    };
    const std::array<eawr::platform::LiveGameOrder, 1> first_gift{{gift(first, second)}};
    step(0, true, first_gift); run(initial + 24);
    const auto transferred = games[0]->battle()->units();
    expect(std::any_of(transferred.begin(), transferred.end(), [&](const auto& unit) {
        return unit.entity_id == id && unit.controller_seat == second;
    }), "the applied gift changes controller before recovery");
    // Rehash the container after damaging the child section: rejection must be
    // due to reconstruction, and it must not publish into the live owners.
    namespace checkpoint = eawr::checkpoint;
    const auto parent_before = games[0]->state_sha256();
    const auto child_before = games[0]->battle()->state_sha256();
    const auto identities = checkpoint::game_identities(games[0]->galaxy().setup());
    auto damaged = require(checkpoint::parse_checkpoint(require(checkpoint::save_game_checkpoint(*games[0], identities))));
    checkpoint::GameLoadOptions load; load.script_requirement = cp::ScriptRequirement::none;
    load.content = pending_fixture_content;
    load.conflict_service = [](const auto&, const auto&, auto) {
        return eawr::core::Result<std::optional<cp::RecordedAdmission>>::success({});
    };
    expect(static_cast<bool>(checkpoint::load_game_checkpoint(require(checkpoint::write_checkpoint(damaged)), identities,
        load, *room.resumed_workers)), "the undamaged active-child checkpoint restores privately");
    bool found_child = false;
    for (auto& section : damaged.sections) if (section.id == static_cast<std::uint16_t>(checkpoint::SectionId::child_state)) {
        section.bytes.push_back(0); found_child = true;
    }
    expect(found_child, "active child checkpoint has a child-state section");
    expect(!checkpoint::load_game_checkpoint(require(checkpoint::write_checkpoint(damaged)), identities, load, *room.resumed_workers),
        "a rehashed corrupt child is refused by the private restore");
    expect(games[0]->state_sha256() == parent_before && games[0]->battle()->state_sha256() == child_before
        && publications[0] == 0 && publications[1] == 0 && publications[2] == 0,
        "corrupt child refusal leaves both live layers and publications untouched");
    const std::array<eawr::platform::LiveGameOrder, 1> second_gift{{gift(second, first)}};
    step(2, true, second_gift);
    const auto before_disconnect = games[2]->battle_record();
    expect(before_disconnect && std::count_if(before_disconnect->commands.begin(), before_disconnect->commands.end(), [](const auto& command) {
        return std::holds_alternative<tactical::GiftPayload>(command.payload);
    }) == 1, "the second gift is still pending when the returning peer disconnects");
    room.network.open[2] = false;
    run(initial + 45, false);
    expect(std::none_of(games[0]->galaxy().seats().begin(), games[0]->galaxy().seats().end(), [&](const auto& seat) {
        return seat.seat_id == second && seat.active;
    }), "the interrupted saved seat departs before recovery");
    room.network.open[2] = true;
    const auto survivor0 = games[0];
    const auto survivor1 = games[1];
    std::array<std::array<unsigned, 256>, 3> recovery_frames{};
    std::optional<std::uint64_t> recovery_barrier;
    room.network.before_send = [&](auto from, auto, std::span<const std::uint8_t> frame) {
        if (frame.empty()) return;
        const auto outer = net::decode_lobby_frame(frame);
        const auto* wrapped = outer ? std::get_if<net::LobbySetupFrame>(&outer.value()) : nullptr;
        const auto payload = wrapped ? std::span<const std::uint8_t>(wrapped->frame) : frame;
        ++recovery_frames[from][payload.front()];
        if (from != 0 || payload.front() != net::gc_recovery_proof_kind) return;
        const auto control = net::decode_galactic_recovery_control(payload);
        if (control && control.value().stage == net::GalacticRecoveryStage::fence)
            recovery_barrier = control.value().barrier;
    };
    const auto frame_summary = [&] {
        std::string result;
        for (std::size_t peer = 0; peer < recovery_frames.size(); ++peer)
            for (std::size_t kind = 0; kind < recovery_frames[peer].size(); ++kind)
                if (recovery_frames[peer][kind]) result += " " + std::to_string(peer) + ":"
                    + std::to_string(kind) + "=" + std::to_string(recovery_frames[peer][kind]);
        return result;
    };
    refuse_restore->store(true);
    auto refused = matches[2]->request_recovery(0);
    bool refused_settled = false;
    for (unsigned round = 0; round < 20000; ++round) {
        for (unsigned peer = 0; peer < 3; ++peer) step(peer, peer < 2 && games[peer]->completed_tick() < initial + 50);
        if (refused.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            const auto reason = refused.get();
            if (reason != Refusal::restore) throw std::runtime_error("injected recovery returned "
                + std::string(net::galactic_recovery_reason(reason)) + "; frames:" + frame_summary());
            refused_settled = true; break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(refused_settled && publications[0] == 0 && publications[1] == 0 && publications[2] == 0
        && games[0] == survivor0 && games[1] == survivor1,
        "failed recovery publishes no partial owner and preserves the survivors");
    if (!refused_settled) throw std::runtime_error("injected recovery refusal did not settle");
    for (unsigned drain = 0; drain < 4; ++drain) for (unsigned peer = 0; peer < 3; ++peer) step(peer, false);
    run(initial + 55, false);
    expect(games[0]->state_sha256() == games[1]->state_sha256() && games[0]->battle() && games[1]->battle(),
        "survivors continue equally in the child after a refused recovery");
    refuse_restore->store(false);
    recovery_frames = {};
    recovery_barrier.reset();
    auto recovered = matches[2]->request_recovery(0);
    bool settled = false;
    for (unsigned round = 0; round < 20000; ++round) {
        for (unsigned peer = 0; peer < 3; ++peer)
            step(peer, peer < 2 && recovery_barrier && games[peer]->completed_tick() < *recovery_barrier);
        if (recovered.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            const auto reason = recovered.get();
            if (reason != Refusal::none) throw std::runtime_error("resumed child recovery refused: "
                + std::string(net::galactic_recovery_reason(reason)) + "; fence="
                + (recovery_barrier ? std::to_string(*recovery_barrier) : "unannounced")
                + "; ticks=" + std::to_string(games[0]->completed_tick()) + ","
                + std::to_string(games[1]->completed_tick()) + "," + std::to_string(games[2]->completed_tick())
                + "; frames:" + frame_summary());
            settled = true; break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    room.network.before_send = {};
    expect(recovery_barrier.has_value(), "the source announces a recovery barrier");
    expect(settled && publications[2] == 1 && publications[0] == 0 && publications[1] == 0,
        "only the returning owner is published");
    if (!settled) throw std::runtime_error("resumed child recovery did not settle");
    for (unsigned drain = 0; drain < 4; ++drain) for (unsigned peer = 0; peer < 3; ++peer) step(peer, false);
    expect(games[0]->completed_tick() == games[1]->completed_tick()
        && games[0]->state_sha256() == games[1]->state_sha256()
        && games[0]->battle() && games[1]->battle()
        && games[0]->battle()->state_sha256() == games[1]->battle()->state_sha256(),
        "parent and child agree at the recovered frontier");
    expect(games[2]->completed_tick() == games[0]->completed_tick()
        && games[2]->state_sha256() == games[0]->state_sha256() && games[2]->battle()
        && games[2]->battle()->state_sha256() == games[0]->battle()->state_sha256(),
        "the returning saved seat agrees at the recovered frontier");
    run(games[0]->completed_tick() + 20);
    expect(games[0]->state_sha256() == games[1]->state_sha256()
        && games[0]->battle() && games[1]->battle()
        && games[0]->battle()->state_sha256() == games[1]->battle()->state_sha256(),
        "recovered parent and active child agree after further play");
    for (const auto& game : games) {
        expect(game->state_sha256() == games[0]->state_sha256() && game->battle()
            && game->battle()->state_sha256() == games[0]->battle()->state_sha256(),
            "every rejoined owner retains equal parent and child state");
        expect(std::any_of(game->galaxy().seats().begin(), game->galaxy().seats().end(), [&](const auto& seat) {
            return seat.seat_id == second && seat.active;
        }), "the original saved human seat is active after rejoin");
        const auto replay = game->battle_record();
        expect(replay && std::count_if(replay->commands.begin(), replay->commands.end(), [](const auto& command) {
            return std::holds_alternative<tactical::GiftPayload>(command.payload);
        }) == 2, "both gifts execute exactly once across recovery");
        const auto recording = game->recording();
        expect(std::count_if(recording.commands.commands.begin(), recording.commands.commands.end(), [](const auto& command) {
            return std::holds_alternative<gc::BattleChoice>(command.payload);
        }) == 1, "the resumed battle choice executes exactly once across recovery");
    }
    expect(rejected.empty(), "valid resumed child commands are not rejected");
    for (auto& driver : drivers) driver->session_ended();
}

// The guest restores a received save on its own thread (#6130), so "agreed" arrives in real time, not in lobby frames:
// keep the frames going, a millisecond apart, until the condition holds or the deadline passes.
template <class Condition>
bool frames_until(GcRoom& room, Condition done, unsigned seconds = 120) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        room.round();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

void production_file_resume(unsigned workers, bool single_player_rules, bool pending_battle = false) {
    using Live = eawr::platform::LiveGameSession;
    using namespace std::chrono_literals;
    namespace tactical = eawr::sim::tactical;
    const unsigned count = pending_battle ? 3U : 2U;
    auto locals = locals_of(count);
    if (pending_battle) for (auto& local : locals) {
        local->content.restore_session = [](const cp::CampaignSetupPlan& plan, std::string_view,
            std::span<const std::uint8_t> saved) -> eawr::core::Result<cp::MpPreparedSession> {
            eawr::checkpoint::GameLoadOptions load;
            load.script_requirement = cp::ScriptRequirement::none;
            load.content = pending_fixture_content;
            load.conflict_service = [](const auto&, const auto&, auto) {
                return eawr::core::Result<std::optional<cp::RecordedAdmission>>::success({});
            };
            const sim::InlineExecutor executor;
            auto restored = eawr::checkpoint::load_game_checkpoint(saved,
                eawr::checkpoint::game_identities(plan.galaxy()), load, executor);
            if (!restored) return eawr::core::Result<cp::MpPreparedSession>::failure(restored.error());
            return eawr::core::Result<cp::MpPreparedSession>::success({
                std::make_shared<cp::GameSession>(std::move(restored).value().session), {}, {}});
        };
    }
    if (single_player_rules) for (auto& local : locals) {
        auto entry = local->data.campaigns.front();
        entry.name = "CoopSinglePlayer"; entry.is_multiplayer = false; entry.starting_active_player = "REBEL";
        local->data.campaigns.push_back(std::move(entry));
    }
    GcRoom room(locals, coop_options(*locals[0]), 1);
    room.resumed_workers = std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers);
    room.resumed_clock_policy = net::GalacticClockPolicy::limited;
    room.admit(1, 1, "guest");
    if (count == 3) room.admit(2, 1, "returning");
    auto roster = require(net::galactic_roster_draft(room.host().snapshot(), locals[0]->catalog()));
    if (single_player_rules) roster.campaign = "CoopSinglePlayer";
    cp::MpSessionTerms terms; terms.seed = match_seed;
    terms.command_quota = static_cast<std::uint32_t>(net::max_batch_commands);
    terms.byte_quota = static_cast<std::uint32_t>(net::max_batch_bytes);
    auto agreed = require(cp::seal_mp_agreement(locals[0]->content, roster, terms));
    // #6156: every peer's plan carries the roster's agreed vote timeout; the live vote setup arms its tick count from it.
    expect(agreed.plan->vote_timeout_seconds() == std::optional<std::uint32_t>{roster.vote_timeout_seconds},
        "the agreed roster's vote timeout reaches the setup plan");
    std::shared_ptr<eawr::platform::CampaignServiceConfiguration> pending_services;
    if (pending_battle) {
        const auto initial = agreed.plan->galaxy();
        pending_services = std::make_shared<eawr::platform::CampaignServiceConfiguration>();
        pending_services->configure = [initial](cp::GameSession& game) {
            game.set_conflict_service([](const auto&, const auto&, auto) {
                return eawr::core::Result<std::optional<cp::RecordedAdmission>>::success({});
            }, pending_fixture_content, {}, cp::ScriptRequirement::none);
            cp::ConflictRecord entry;
            entry.id = game.galaxy().next_entity_id(); entry.planet = initial.planets.front().id;
            entry.invader = 1; entry.defender = 2;
            entry.tactical.content_identity = initial.content_identity;
            entry.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
            entry.tactical.campaign.emplace();
            for (const auto player : {1U, 2U}) {
                tactical::EconomyPlayer account; account.player = player; account.population_cap = 20; account.credit_cap = Fixed{};
                entry.tactical.campaign->players.push_back(account);
            }
            // The lobby fixture installs no placement services, so the planned galaxy has no fleets and
            // nothing to deploy. The conflict carries escort forces instead: they need no galactic parent,
            // and each human seat of the invading side gets one root to control.
            constexpr gc::TypeId escort_type = 2;
            const std::array<gc::PlayerId, 4> owners{1, 1, 1, 2};
            entry.tactical.first_free_entity_id = entry.id + 1 + owners.size();
            for (std::size_t index = 0; index < owners.size(); ++index) {
                const auto id = entry.id + 1 + index;
                cp::ConflictForce force{id, escort_type, owners[index], 0, cp::ForceRole::escort};
                entry.forces.push_back(force);
                entry.tactical.units.push_back({id, escort_type, owners[index]});
            }
            for (const auto& seat : initial.seats) if (seat.active && seat.player <= 2)
                entry.seats.push_back({seat.seat_id, seat.player});
            return game.queue_conflict(std::move(entry), {cp::ConflictRoute::pending_tactical, false, false});
        };
    }
    std::vector<net::SetupSeat> bindings;
    for (unsigned peer = 0; peer < count; ++peer) bindings.push_back({peer, roster.seats[peer].seat_id, roster.seats[peer].player});

    LivePipes pipes(count);
    std::vector<std::unique_ptr<LivePipe>> transports(count);
    std::vector<std::unique_ptr<viewer::GalacticLanMatch>> matches(count);
    std::vector<std::unique_ptr<Live>> live(count);
    const auto directory = std::filesystem::temp_directory_path() /
        ("eawr-lobby-live-resume-" + std::to_string(workers) + (single_player_rules ? "-sp" : "-mp"));
    std::filesystem::create_directories(directory);
    std::filesystem::remove(directory / "host.eawrsave");
    for (unsigned peer = 0; peer < count; ++peer) {
        transports[peer] = std::make_unique<LivePipe>(pipes, peer);
        viewer::GalacticLanOptions lan;
        for (const auto& binding : bindings) lan.seats.push_back({binding.seat, true});
        matches[peer] = std::make_unique<viewer::GalacticLanMatch>(*transports[peer], lan);
        Live::Options options; options.workers = workers; options.pacing = Live::Pacing::driven;
        options.step_driver = matches[peer]->driver(); options.saves.directory = directory.string();
        options.services = pending_services;
        eawr::platform::LiveSaveCampaignSource source;
        source.campaign = roster.campaign; source.difficulty = roster.difficulty; source.multiplayer = true;
        for (const auto& player : roster.players) source.factions.push_back({player.player, player.faction});
        options.saves.campaign = std::move(source);
        live[peer] = require(Live::start(agreed.plan->galaxy(), std::move(options)));
    }
    for (auto& peer : live) peer->advance_to(5);
    for (auto& peer : live) expect(peer->wait_for(5, 30s), "live save fixture reaches tick five");
    if (pending_battle) for (auto& peer : live)
        expect(peer->frame().pending_conflict && !peer->frame().battle, "player save starts at a pending conflict");
    eawr::platform::LiveSaveRequest request; request.seat = bindings[1].seat; request.name = "guest";
    const auto rejected = live[1]->request_save(request);
    expect(rejected && live[1]->save_outcomes().back().phase == eawr::platform::LiveSavePhase::rejected,
        "the guest cannot publish a save");
    request.seat = bindings[0].seat; request.name = "host";
    const auto token = live[0]->request_save(request);
    constexpr std::uint64_t horizon = 60;
    for (auto& peer : live) peer->advance_to(horizon);
    for (auto& peer : live) expect(peer->wait_for(horizon, 30s), "live peers continue after saving");
    expect(live[0]->wait_save(token, 30s), "host file publication finishes");
    const auto outcome = live[0]->save_outcomes().back();
    expect(outcome.phase == eawr::platform::LiveSavePhase::completed && outcome.completed_tick < horizon,
        "host publishes the agreed intermediate checkpoint");
    const auto hashes = live[0]->tick_hashes();
    for (const auto& peer : live)
        expect(hashes == peer->tick_hashes() && hashes.size() >= horizon, "uninterrupted live peers agree");
    auto bytes = std::make_shared<const Bytes>(require(eawr::platform::read_saved_game_bytes(directory.string(), "host")));
    for (auto& peer : live) peer->stop();
    auto candidate = require(room.peers[0].backend->inspect_checkpoint(*bytes));
    expect(candidate.seed == match_seed && candidate.saved.completed_tick == outcome.completed_tick,
        "inspection reads the real host file and its saved frontier");
    auto corrupted = *bytes; corrupted.back() ^= 1;
    expect(!room.peers[0].backend->inspect_checkpoint(corrupted), "corrupt host file is refused");
    if (single_player_rules) {
        auto options = require(net::decode_galactic_lobby_game(room.host().snapshot().game));
        options.campaign = candidate.original_roster.campaign;
        auto game = require(net::encode_galactic_lobby_game(options));
        require(room.host().configure_game(std::move(game), room.host().snapshot().seats, room.revision()));
        room.rounds(3);
        for (const auto& peer : room.peers)
            expect(peer.backend->validate(room.host().snapshot(), true).has_value(),
                "an eligible SP entry can start as a new co-op game before checkpoint selection");
    }
    // Exercise the saved logical identities against the actual published file,
    // before any checkpoint selection becomes visible in the room.
    auto unknown = bindings; unknown.back().seat = sim::SeatId{9999};
    const auto unknown_result = room.peers[0].backend->select_checkpoint(
        room.host().snapshot(), candidate.original_roster, unknown, bytes, 1);
    expect(!unknown_result && unknown_result.error().message.find("unknown_saved_human_seat") != std::string::npos,
        "the actual host file refuses an unknown saved seat");
    auto faction = bindings; faction.back().player = 2;
    const auto faction_result = room.peers[0].backend->select_checkpoint(
        room.host().snapshot(), candidate.original_roster, faction, bytes, 1);
    expect(!faction_result && faction_result.error().message.find("saved_faction_mismatch") != std::string::npos,
        "the actual host file refuses a different saved faction");
    auto duplicate = bindings; duplicate.back().seat = duplicate.front().seat;
    const auto duplicate_result = room.peers[0].backend->select_checkpoint(
        room.host().snapshot(), candidate.original_roster, duplicate, bytes, 1);
    expect(!duplicate_result && duplicate_result.error().message.find("duplicate_saved_seat") != std::string::npos,
        "the actual host file refuses a duplicate saved seat");
    expect(!room.peers[0].backend->checkpoint_view() && room.peers[0].backend->initial_tick() == 0,
        "refused mappings never publish a partial restored owner");
    for (auto& peer : room.peers) peer.backend->acceptance_final_tick(pending_battle ? 300 : horizon);
    require(room.peers[0].backend->select_checkpoint(room.host().snapshot(), candidate.original_roster, bindings, bytes, 1));
    frames_until(room, [&] { return room.peers[0].backend->checkpoint_progress().agreed; });
    expect(room.peers[0].backend->checkpoint_progress().agreed, "both peers accept the transferred file");
    room.all_ready();
    require(room.host().start(room.revision())); room.rounds(3);
    for (unsigned peer = 1; peer < count; ++peer) require(room.sessions[peer].accept_setup());
    for (unsigned i = 0; i < 1000 && !std::all_of(room.sessions.begin(), room.sessions.end(),
        [](const auto& session) { return session.result_ready(); }); ++i) room.round();
    std::vector<net::LobbyResult> results;
    for (auto& session : room.sessions) results.push_back(require(session.take_result()));
    for (std::size_t i = 0; i < room.peers.size(); ++i) {
        const auto& peer = room.peers[i];
        expect(peer.agreement()->session->completed_tick() == outcome.completed_tick
            && results[i].setup().adapter().state_hash() == hashes.at(outcome.completed_tick - 1)
            && peer.agreement()->plan->galaxy().uses_network_transport()
            && peer.agreement()->plan->rules_flavour() == (single_player_rules
                ? cp::CampaignRulesFlavour::single_player : cp::CampaignRulesFlavour::multiplayer),
            "restored N equals the uninterrupted live owner");
        if (pending_battle) expect(peer.agreement()->session->pending_conflict() != nullptr,
            "the real host file restores its pending conflict before the choice");
    }
    if (pending_battle) {
        recover_resumed_child(room, std::move(results), bytes, workers);
        eawr::testing::remove_temp_directory(directory);
        return;
    }
    for (unsigned i = 0; i < 2000; ++i) {
        ++room.network.round;
        for (auto& result : results) require(result.setup().session().pump(room.network.round));
    }
    for (std::size_t i = 0; i < room.peers.size(); ++i)
        expect(room.peers[i].agreement()->session->completed_tick() == horizon
            && results[i].setup().adapter().state_hash() == hashes.at(horizon - 1),
            "resumed N+k equals the uninterrupted live owner");
    eawr::testing::remove_temp_directory(directory);
}

std::string checkpoint_room_resume(unsigned workers, unsigned count, bool relay, bool departed = false, unsigned lobby_failure = 0,
    bool single_player_rules = false) {
    auto locals = locals_of(count);
    for (auto& local : locals) {
        auto campaign = local->data.campaigns.front();
        campaign.name = "CoopSinglePlayer"; campaign.is_multiplayer = false;
        campaign.starting_active_player = "REBEL";
        local->data.campaigns.push_back(std::move(campaign));
    }
    GcRoom room(locals, coop_options(*locals[0]), 1);
    room.resumed_workers = std::make_shared<eawr::platform::ThreadWorkerAdapter>(workers);
    room.resumed_clock_policy = net::GalacticClockPolicy::limited;
    for (unsigned peer = 1; peer < count; ++peer) room.admit(peer, 1, "human" + std::to_string(peer));
    auto roster = require(net::galactic_roster_draft(room.host().snapshot(), locals[0]->catalog()));
    if (single_player_rules) roster.campaign = "CoopSinglePlayer";
    if (departed) {
        for (auto& seat : roster.seats) if (seat.kind != sim::SeatKind::human) ++seat.seat_id.value;
        roster.seats.insert(roster.seats.begin() + static_cast<std::ptrdiff_t>(count),
            gc::Seat{sim::SeatId{count + 1}, 1, sim::SeatKind::human, count, true, 1});
    }
    // Logical saved IDs deliberately differ from the new room's ephemeral IDs.
    for (auto& seat : roster.seats) seat.seat_id.value += 10;
    for (auto& team : roster.teams) team.lead.value += 10;
    cp::MpSessionTerms terms; terms.seed = match_seed;
    terms.command_quota = static_cast<std::uint32_t>(net::max_batch_commands); terms.byte_quota = static_cast<std::uint32_t>(net::max_batch_bytes);
    auto source = require(cp::seal_mp_agreement(locals[0]->content, roster, terms));
    net::GalacticAdapterOptions clock_options; clock_options.clock_policy = net::GalacticClockPolicy::limited;
    net::GalacticSessionAdapter source_adapter(*source.session, *room.resumed_workers, clock_options);
    constexpr std::uint64_t boundary = 7, horizon = 20;
    if (departed) require(source.session->submit({{0, sim::SeatId{count + 11}, 0}, gc::SeatLeave{}}));
    for (std::uint64_t tick = 0; tick < boundary; ++tick) require(source_adapter.step());
    auto sequences = source.session->producer_sequences();
    std::vector<net::SetupSeat> bindings;
    for (unsigned peer = 0; peer < count; ++peer) {
        const sim::SeatId seat{peer + 11}; sequences[seat] = 100 + peer;
        bindings.push_back({peer, seat, 1});
        room.peers[peer].backend->acceptance_final_tick(horizon);
    }
    require(source.session->retain_network_producer_sequences(sequences));
    require(source.session->retain_network_resume_state(source_adapter.checkpoint_network_state()));
    auto saved = require(eawr::checkpoint::parse_checkpoint(require(eawr::checkpoint::save_game_checkpoint(*source.session,
        eawr::checkpoint::game_identities(source.session->galaxy().setup())))));
    eawr::platform::LiveSaveCampaign description;
    description.campaign = roster.campaign; description.difficulty = roster.difficulty; description.multiplayer = true;
    description.faction = roster.players.front().faction; description.seat = bindings.front().seat.value;
    saved.sections.push_back(eawr::platform::save_detail::encode_campaign(description));
    const auto bytes = std::make_shared<const Bytes>(require(eawr::checkpoint::write_checkpoint(saved)));
    auto candidate = require(room.peers[0].backend->inspect_checkpoint(*bytes));
    if (single_player_rules) {
        expect(room.peers[0].backend->campaigns().size() == 2,
            "the eligible SP campaign is offered in the co-op catalog");
        auto wrong = roster; wrong.campaign = "Multiplayer";
        expect(!room.peers[0].backend->select_checkpoint(room.host().snapshot(), wrong, bindings, bytes, 1),
            "saved SP setup cannot be restored as the MP entry");
    }
    if (!single_player_rules) {
        auto wrong = roster; wrong.campaign = "CoopSinglePlayer";
        expect(!room.peers[0].backend->select_checkpoint(room.host().snapshot(), wrong, bindings, bytes, 1),
            "saved MP setup cannot be restored as the SP entry");
    }
    candidate.original_roster.tick_zero = roster.tick_zero;
    // #6156: this fixture arms no vote service, so its save recorded no vote timeout and the inspected roster carries none.
    expect(candidate.original_roster.vote_timeout_seconds == 0 && roster.vote_timeout_seconds == 15,
        "a save without a vote service recorded no timeout; the room's roster has the default");
    candidate.original_roster.vote_timeout_seconds = roster.vote_timeout_seconds;
    expect(candidate.original_roster == roster && candidate.seed == match_seed, "UI inspection recovers original saved roster and seed without new-game placement");
    expect(candidate.original_roster.players.size() == 4
        && candidate.original_roster.players[2].faction == "UNDERWORLD"
        && candidate.original_roster.players[3].faction == "NEUTRAL"
        && std::any_of(candidate.original_roster.seats.begin(), candidate.original_roster.seats.end(), [](const auto& seat) {
            return seat.player == 3 && seat.kind == sim::SeatKind::ai;
        }), "saved roster retains the non-playable AI and neutral owners");
    if (workers == 1 && !relay) {
        auto wrong_seed_room = room.host().snapshot();
        auto wrong_seed_options = require(net::decode_galactic_lobby_game(wrong_seed_room.game));
        ++wrong_seed_options.seed;
        wrong_seed_room.game = require(net::encode_galactic_lobby_game(wrong_seed_options));
        expect(!room.peers[0].backend->prepare_checkpoint(wrong_seed_room, roster, bindings, bytes, 1),
            "a room seed that differs from the saved setup is refused");
        auto missing = bindings; missing.pop_back();
        const auto missing_result = room.peers[0].backend->select_checkpoint(room.host().snapshot(), roster, missing, bytes, 1);
        expect(!missing_result && missing_result.error().message.find("missing_human_seat") != std::string::npos, "missing saved human refuses by name");
        auto unknown = bindings; unknown.back().seat = sim::SeatId{9999};
        const auto unknown_result = room.peers[0].backend->select_checkpoint(room.host().snapshot(), roster, unknown, bytes, 1);
        expect(!unknown_result && unknown_result.error().message.find("unknown_saved_human_seat") != std::string::npos, "unknown saved seat is refused");
        auto faction = bindings; faction.back().player = 2;
        const auto faction_result = room.peers[0].backend->select_checkpoint(room.host().snapshot(), roster, faction, bytes, 1);
        expect(!faction_result && faction_result.error().message.find("saved_faction_mismatch") != std::string::npos, "saved faction mismatch is refused");
        auto duplicate = bindings; duplicate.back().seat = duplicate.front().seat;
        const auto duplicate_result = room.peers[0].backend->select_checkpoint(room.host().snapshot(), roster, duplicate, bytes, 1);
        expect(!duplicate_result && duplicate_result.error().message.find("duplicate_saved_seat") != std::string::npos, "duplicate saved human refuses by name");
        auto extra = bindings; extra.push_back({count, sim::SeatId{99}, 1});
        const auto extra_result = room.peers[0].backend->select_checkpoint(room.host().snapshot(), roster, extra, bytes, 1);
        expect(!extra_result && extra_result.error().message.find("extra_human_seat") != std::string::npos, "extra human refuses by name");
        if (departed) {
            auto inactive = bindings; inactive.back().seat = sim::SeatId{count + 11};
            const auto inactive_result = room.peers[0].backend->select_checkpoint(room.host().snapshot(), roster, inactive, bytes, 1);
            expect(!inactive_result && inactive_result.error().message.find("inactive_saved_seat") != std::string::npos, "departed saved human cannot reactivate through mapping");
        }
    }
    room.all_ready();
    if (workers == 1 && !relay) {
        auto prepared = require(room.peers[0].backend->prepare_checkpoint(room.host().snapshot(), roster, bindings, bytes, 1));
        auto display_only = room.host().snapshot();
        display_only.mounted_mods = {3, 1, 0, 0, 0, 0};
        const auto with_mods = require(room.peers[0].backend->prepare_checkpoint(display_only, roster, bindings, bytes, 1));
        expect(with_mods.manifest.offer.setup == prepared.manifest.offer.setup
            && with_mods.manifest.offer.identity.build == prepared.manifest.offer.identity.build
            && with_mods.manifest.offer.identity.content == prepared.manifest.offer.identity.content
            && with_mods.agreement->session->state_sha256() == prepared.agreement->session->state_sha256(),
            "lobby mod descriptions do not affect checkpoint setup, identity or simulation hash");
        expect(!room.peers[0].backend->checkpoint_view(), "preparation does not publish a checkpoint");
        auto changed = room.host().snapshot();
        ++changed.revision;
        expect(!room.peers[0].backend->commit_checkpoint(std::move(prepared), changed),
            "a changed room refuses an asynchronously prepared checkpoint");
        expect(!room.peers[0].backend->checkpoint_view(), "stale preparation leaves the room unchanged");
    }
    if (lobby_failure == 2) room.network.fault = [](auto, auto, std::span<const std::uint8_t> frame) {
        eawr::testing::FakeDelivery delivery; delivery.drop = !frame.empty() && frame.front() == net::save_chunk_kind; return delivery;
    };
    require(room.peers[0].backend->select_checkpoint(room.host().snapshot(), roster, bindings, bytes, 1));
    if (lobby_failure == 2) {
        room.rounds(3);
        expect(room.peers[1].backend->checkpoint_manifest() && !room.peers[1].backend->checkpoint_progress().restored,
            "failure before private restore retains a pending manifest");
    }
    if (relay) room.network.emulate(eawr::testing::wan_profiles[1]);
    if (lobby_failure != 2) {
        frames_until(room, [&] { return room.peers[0].backend->checkpoint_progress().agreed; });
        expect(room.peers[0].backend->checkpoint_progress().agreed, "all restored proofs agree before Start");
    }
    if (lobby_failure) {
        if (lobby_failure == 3) {
            require(room.host().kick(2, room.revision())); room.drop_last();
            const auto survived = room.sessions[1].pump(++room.network.round);
            expect(static_cast<bool>(survived), "saved-seat refusal preserves surviving guest staging room");
            expect(room.sessions[1].snapshot().seats.size() == 2, "survivor accepts reduced authoritative roster");
            room.rounds(3);
            for (unsigned peer = 0; peer < room.sessions.size(); ++peer) {
                const auto progress = room.peers[peer].backend->checkpoint_progress();
                expect(progress.refusal && progress.refusal->message.find("missing_human_seat") != std::string::npos,
                    "missing saved human is a named refusal on every survivor");
            }
            const auto started = room.host().start(room.revision());
            expect(!started && started.error().message.find("missing_human_seat") != std::string::npos,
                "incomplete saved mapping cannot activate setup");
        } else {
            Bytes cancel{net::save_cancel_kind, 1,0,0,0,0,0,0,0, 14,0,0,0};
            const std::string reason = "peer_cancelled"; cancel.insert(cancel.end(), reason.begin(), reason.end());
            require(room.transports[1]->send(0, cancel)); room.rounds(3);
            expect(room.peers[0].backend->checkpoint_progress().refusal.has_value(), "host receives peer cancellation");
        }
        require(room.sessions[1].chat("still in the staging room")); room.rounds(3);
        expect(!room.host().chat_history().empty(), "chat remains usable after saved-seat/transfer refusal");
        room.peers[0].backend->clear_checkpoint(); room.network.fault = {}; room.rounds(3);
        for (unsigned peer = 0; peer < room.sessions.size(); ++peer) {
            expect(!room.peers[peer].backend->checkpoint_manifest() && room.peers[peer].backend->initial_tick() == 0,
                "explicit New Campaign clears even an already-refused transfer on every peer");
            expect(room.sessions[peer].snapshot().phase == net::LobbyPhase::staging && !room.sessions[peer].setup_phase(),
                "refusal never partially activates a setup");
        }
        room.commit();
        for (auto& session : room.sessions) {
            auto launch = require(session.take_result());
            expect(launch.setup().adapter().completed_tick() == 0, "ordinary new-game launch succeeds after refused checkpoint is cleared");
        }
        return "cleared";
    }
    for (const auto& seat : room.host().snapshot().seats) expect(!seat.ready, "checkpoint selection clears every ready vote");
    for (unsigned peer = 0; peer < count; ++peer) {
        expect(room.peers[peer].backend->initial_tick() == boundary, "every peer restored completed N");
        expect(room.peers[peer].backend->producer_sequences() == sequences, "logical producer continuation agrees");
    }
    // WAN delivery needs more than the fixture's three-round readiness helper.
    room.round(2000);
    for (auto& session : room.sessions) require(session.request(net::LobbyAction::ready, room.revision()));
    room.rounds(300);
    require(room.host().start(room.revision())); room.rounds(300);
    for (unsigned peer = 1; peer < count; ++peer) require(room.sessions[peer].accept_setup());
    for (unsigned round = 0; round < 1000 && !std::all_of(room.sessions.begin(), room.sessions.end(),
        [](const auto& session) { return session.result_ready(); }); ++round) room.round();
    std::vector<net::LobbyResult> results;
    for (auto& session : room.sessions) results.push_back(require(session.take_result()));
    for (unsigned peer = 0; peer < count; ++peer) {
        expect(results[peer].setup().adapter().completed_tick() == boundary, "setup hash barrier never steps restored owner");
        expect(room.peers[peer].agreement()->session->state_sha256() == source.session->state_sha256()
            && results[peer].setup().adapter().state_hash() == source_adapter.state_hash(), "restored N equals the source state and limited clock");
        expect(results[peer].setup().offer().seats == bindings, "authenticated peers retain assigned saved logical seats");
    }
    std::vector<net::SeatInput> source_inputs;
    for (unsigned peer = 0; peer < count; ++peer) {
        const auto admission_tick = std::max(results[peer].setup().session().next_seal(), boundary + 3);
        const auto key = require(results[peer].setup().session().submit([](auto stamp) {
            return net::encode_galactic_input(stamp, gc::PauseGalaxy{});
        }));
        expect(key.sequence == sequences.at(bindings[peer].seat) + 1 && key.tick == admission_tick,
            "actual resumed campaign command follows saved inclusive maximum at the preserved lead");
        source_inputs.push_back({key, require(net::encode_galactic_input(key, gc::PauseGalaxy{}))});
    }
    for (unsigned round = 0; round < 2000; ++round) {
        ++room.network.round;
        for (auto& result : results) require(result.setup().session().pump(room.network.round));
    }
    std::sort(source_inputs.begin(), source_inputs.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    for (std::uint64_t tick = boundary; tick < horizon; ++tick) {
        std::vector<net::SeatInput> inputs;
        for (const auto& input : source_inputs) if (input.key.tick == tick) inputs.push_back(input);
        require(source_adapter.install(inputs));
        require(source_adapter.step());
    }
    for (std::size_t peer = 0; peer < room.peers.size(); ++peer)
        expect(room.peers[peer].agreement()->session->completed_tick() == horizon
            && room.peers[peer].agreement()->session->state_sha256() == source.session->state_sha256()
            && results[peer].setup().adapter().state_hash() == source_adapter.state_hash(), "continued N+k equals uninterrupted campaign and limited clock");
    return source.session->state_sha256();
}


// The guest's restore of a received save runs on its own thread (#6130): the lobby frame never waits for it, a selection
// cleared or replaced while it runs leaves nothing of the old one, and only the newest selection is installed. The restore
// callback of the guest blocks on a gate the test opens, so the thread is provably still running while the frames go on.
struct RestoreGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool open{};
    std::atomic<int> entered{};
    void wait() {
        std::unique_lock lock(mutex);
        condition.wait_for(lock, std::chrono::seconds(30), [&] { return open; }); // a regression times out, it does not hang
    }
    void release() {
        { const std::lock_guard lock(mutex); open = true; }
        condition.notify_all();
    }
};

void guest_restore_runs_off_the_ui_thread() {
    const auto gate = std::make_shared<RestoreGate>();
    auto locals = locals_of(2);
    // A held or dropped restore thread keeps raw pointers into these peers' data until it ends: keep them for the process.
    static std::vector<std::vector<std::shared_ptr<Local>>> kept_alive;
    kept_alive.push_back(locals);
    locals[1]->content.restore_session = [gate](const cp::CampaignSetupPlan& plan, std::string_view,
        std::span<const std::uint8_t> saved) -> eawr::core::Result<cp::MpPreparedSession> {
        ++gate->entered; gate->wait();
        eawr::checkpoint::GameLoadOptions load; load.script_requirement = cp::ScriptRequirement::none;
        const sim::InlineExecutor executor;
        auto restored = eawr::checkpoint::load_game_checkpoint(saved, eawr::checkpoint::game_identities(plan.galaxy()), load, executor);
        if (!restored) return eawr::core::Result<cp::MpPreparedSession>::failure(restored.error());
        return eawr::core::Result<cp::MpPreparedSession>::success({
            std::make_shared<cp::GameSession>(std::move(restored).value().session), {}, {}});
    };
    GcRoom room(locals, coop_options(*locals[0]), 1);
    room.resumed_workers = std::make_shared<eawr::platform::ThreadWorkerAdapter>(1);
    room.resumed_clock_policy = net::GalacticClockPolicy::limited;
    room.admit(1, 1, "guest");
    auto roster = require(net::galactic_roster_draft(room.host().snapshot(), locals[0]->catalog()));
    for (auto& seat : roster.seats) seat.seat_id.value += 10;
    for (auto& team : roster.teams) team.lead.value += 10;
    cp::MpSessionTerms terms; terms.seed = match_seed;
    terms.command_quota = static_cast<std::uint32_t>(net::max_batch_commands); terms.byte_quota = static_cast<std::uint32_t>(net::max_batch_bytes);
    auto source = require(cp::seal_mp_agreement(locals[0]->content, roster, terms));
    net::GalacticAdapterOptions clock_options; clock_options.clock_policy = net::GalacticClockPolicy::limited;
    net::GalacticSessionAdapter source_adapter(*source.session, *room.resumed_workers, clock_options);
    for (std::uint64_t tick = 0; tick < 7; ++tick) require(source_adapter.step());
    auto sequences = source.session->producer_sequences();
    std::vector<net::SetupSeat> bindings;
    for (unsigned peer = 0; peer < 2; ++peer) {
        const sim::SeatId seat{peer + 11}; sequences[seat] = 100 + peer;
        bindings.push_back({peer, seat, 1});
    }
    require(source.session->retain_network_producer_sequences(sequences));
    require(source.session->retain_network_resume_state(source_adapter.checkpoint_network_state()));
    auto saved = require(eawr::checkpoint::parse_checkpoint(require(eawr::checkpoint::save_game_checkpoint(*source.session,
        eawr::checkpoint::game_identities(source.session->galaxy().setup())))));
    eawr::platform::LiveSaveCampaign description;
    description.campaign = roster.campaign; description.difficulty = roster.difficulty; description.multiplayer = true;
    description.faction = roster.players.front().faction; description.seat = bindings.front().seat.value;
    saved.sections.push_back(eawr::platform::save_detail::encode_campaign(description));
    const auto bytes = std::make_shared<const Bytes>(require(eawr::checkpoint::write_checkpoint(saved)));
    room.all_ready();
    const auto wait_for_entries = [&](int count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (gate->entered < count && std::chrono::steady_clock::now() < deadline) room.round();
        return gate->entered >= count;
    };
    const auto guest = [&]() -> viewer::GalacticLobbyBackend& { return *room.peers[1].backend; };
    const auto host = [&]() -> viewer::GalacticLobbyBackend& { return *room.peers[0].backend; };

    // 1. The restore is blocked: the frames go on, nothing is restored or agreed, and the wait is on the thread, not the frame.
    require(host().select_checkpoint(room.host().snapshot(), roster, bindings, bytes, 1));
    expect(wait_for_entries(1), "the guest's restore starts on its own thread");
    const auto before = std::chrono::steady_clock::now();
    room.rounds(60);
    const auto blocked_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before).count();
    expect(blocked_ms < 5000, "lobby frames do not wait for a running restore (60 frames took " + std::to_string(blocked_ms) + " ms)");
    expect(!guest().checkpoint_progress().restored && guest().initial_tick() == 0, "the guest is not restored while its restore runs");
    expect(!host().checkpoint_progress().agreed, "the host does not see agreement while the guest's restore runs");

    // 2. The selection is cleared while the restore runs: the finished restore leaves no selection behind.
    host().clear_checkpoint();
    room.rounds(6);
    gate->release();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    room.rounds(30);
    expect(!guest().checkpoint_manifest() && !guest().checkpoint_view() && guest().initial_tick() == 0,
        "a selection cleared while the restore ran installs nothing when the restore finishes");
    expect(!host().checkpoint_progress().agreed, "no agreement survives a cleared selection");

    // 2b. A failure that persists across frames is traced once, not once per frame: the room changes under a selection, and
    // the host keeps refusing its binding.
    {
        std::vector<std::string> cancels;
        host().set_checkpoint_trace([&](std::string_view event, const std::string& detail) {
            if (event == "cancel" || event == "select-failed") cancels.push_back(std::string(event) + " " + detail);
        });
        require(host().select_checkpoint(room.host().snapshot(), roster, bindings, bytes, host().selection_epoch() + 1));
        frames_until(room, [&] { return host().checkpoint_progress().agreed; });
        expect(host().checkpoint_progress().agreed, "the reselected checkpoint is agreed before the room changes");
        require(room.host().kick(1, room.revision())); room.drop_last();
        for (unsigned frame = 0; frame < 60; ++frame) room.round();
        expect(cancels.size() <= 1, "a binding refusal that persists is traced once, not once per frame (" + std::to_string(cancels.size()) + " lines)");
        host().set_checkpoint_trace({});
    }

    // 3. A second selection replaces the first while the first restore still runs: only the newer epoch is installed.
    {
        const auto replace_gate = std::make_shared<RestoreGate>();
        auto fresh = locals_of(2);
        kept_alive.push_back(fresh);
        fresh[1]->content.restore_session = [replace_gate](const cp::CampaignSetupPlan& plan, std::string_view,
            std::span<const std::uint8_t> data) -> eawr::core::Result<cp::MpPreparedSession> {
            const int entry = ++replace_gate->entered;
            if (entry == 1) replace_gate->wait(); // only the first (older) restore is held; the newer one runs straight through
            eawr::checkpoint::GameLoadOptions load; load.script_requirement = cp::ScriptRequirement::none;
            const sim::InlineExecutor executor;
            auto restored = eawr::checkpoint::load_game_checkpoint(data, eawr::checkpoint::game_identities(plan.galaxy()), load, executor);
            if (!restored) return eawr::core::Result<cp::MpPreparedSession>::failure(restored.error());
            return eawr::core::Result<cp::MpPreparedSession>::success({
                std::make_shared<cp::GameSession>(std::move(restored).value().session), {}, {}});
        };
        GcRoom second(fresh, coop_options(*fresh[0]), 1);
        second.resumed_workers = std::make_shared<eawr::platform::ThreadWorkerAdapter>(1);
        second.resumed_clock_policy = net::GalacticClockPolicy::limited;
        second.admit(1, 1, "guest");
        second.all_ready();
        require(second.peers[0].backend->select_checkpoint(second.host().snapshot(), roster, bindings, bytes, 1));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (replace_gate->entered < 1 && std::chrono::steady_clock::now() < deadline) second.round();
        expect(replace_gate->entered >= 1, "the first selection's restore is running");
        second.all_ready();
        require(second.peers[0].backend->select_checkpoint(second.host().snapshot(), roster, bindings, bytes, 2));
        frames_until(second, [&] { return second.peers[0].backend->checkpoint_progress().agreed; });
        expect(second.peers[0].backend->checkpoint_progress().agreed, "the newer selection restores while the older restore is still held");
        const auto manifest = second.peers[1].backend->checkpoint_manifest();
        expect(manifest && manifest->epoch == 2, "the guest holds the newer selection");
        replace_gate->release();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        second.rounds(30);
        const auto after = second.peers[1].backend->checkpoint_manifest();
        expect(after && after->epoch == 2 && second.peers[0].backend->checkpoint_progress().agreed,
            "the older restore finishing late does not replace the newer selection");
    }
}

// ---- #6134: a guest picks any free playable faction, still two human sides ----

void free_faction_choice() {
    Local three_factions;
    three_factions.data.factions[2].is_playable = true; // REBEL, EMPIRE and UNDERWORLD are all playable
    const auto catalog = three_factions.catalog();
    const auto& campaign = catalog.front();
    const auto options = require(net::default_galactic_lobby_options(campaign, difficulty, match_seed));
    expect(options.rows[0].human && options.rows[1].human && !options.rows[2].human, "the room plans the first two playable factions");
    const auto before = snapshot_of(options, {seat_of(0, 1, options, 1, "host"), seat_of(1, 2, options, 2, "guest")});
    // The pure rule.
    const auto moved = net::galactic_lobby_options_with_seat_on(options, campaign, before, 1, 3);
    expect(moved.has_value() && moved->rows[0].human && !moved->rows[1].human && moved->rows[2].human,
        "the guest moving to the free faction opens it and closes the row it leaves");
    expect(net::galactic_lobby_options_with_seat_on(options, campaign, before, 1, 2) == options, "a planned row stays as it is");
    expect(!net::galactic_lobby_options_with_seat_on(options, campaign, before, 1, 4).has_value(), "a neutral faction cannot be taken");
    expect(!net::galactic_lobby_options_with_seat_on(options, campaign, before, 1, 9).has_value(), "an unknown row cannot be taken");
    const auto shared = snapshot_of(options, {seat_of(0, 1, options, 2, "host"), seat_of(1, 2, options, 2, "guest")});
    expect(!net::galactic_lobby_options_with_seat_on(options, campaign, shared, 1, 3).has_value(),
        "a move that would plan a third human side is refused (the other seat still holds the row it leaves)");
    Local two_playable;
    expect(!net::galactic_lobby_options_with_seat_on(options, two_playable.catalog().front(), before, 1, 3).has_value(),
        "a faction the campaign does not let a player hold cannot be taken");
    // The choice hook rewrites the room.
    auto candidate = before;
    candidate.seats[1].selection.slot = 3; candidate.seats[1].selection.faction = "UNDERWORLD"; candidate.seats[1].selection.team = 0;
    expect(net::follow_galactic_choice(before, candidate, 1, catalog).has_value(), "the choice hook accepts a free faction");
    const auto planned = require(net::decode_galactic_lobby_game(candidate.game));
    expect(planned.rows[0].human && !planned.rows[1].human && planned.rows[2].human, "the hook reshapes the rows");
    expect(candidate.seats[0].selection.team == 1 && candidate.seats[1].selection.team == 2 && candidate.seats[1].selection.faction == "UNDERWORLD",
        "every seat's side and faction follow its row");
    expect(net::validate_galactic_lobby(candidate, false, catalog).has_value(), "the reshaped room validates");
    auto crowded = shared;
    crowded.seats[1].selection.slot = 3; crowded.seats[1].selection.faction = "UNDERWORLD";
    const auto refusal = net::follow_galactic_choice(shared, crowded, 1, catalog);
    expect(!refusal && refusal.error().message.find("at most two human sides") != std::string::npos,
        "a third side is refused with the reason");
    // The faction dropdown offers the same rows the hook accepts.
    const auto offered = eawr::viewer::galactic::faction_choices(options, eawr::data::Catalog{}, nullptr, campaign, before, 1);
    expect(offered.size() == 3 && offered[2].row == 2, "the guest's dropdown lists the free playable faction");
    const auto host_offered = eawr::viewer::galactic::faction_choices(options, eawr::data::Catalog{}, nullptr, campaign, shared, 1);
    expect(host_offered.size() == 2, "a faction that would make a third side is not listed");
    expect(eawr::viewer::galactic::faction_choice_player(options, 2, campaign) == std::optional<gc::PlayerId>{3}
        && !eawr::viewer::galactic::faction_choice_player(options, 3, campaign).has_value(),
        "the free faction maps to its player, a neutral row to none");

    // A live room: the guest picks the free faction through the lobby, the host's session reshapes the room.
    std::vector<std::shared_ptr<Local>> locals;
    for (int i = 0; i < 2; ++i) { locals.push_back(std::make_shared<Local>()); locals.back()->data.factions[2].is_playable = true; }
    GcRoom room(locals, require(net::default_galactic_lobby_options(locals[0]->catalog().front(), difficulty, match_seed)), 1, 0, true);
    room.admit(1, 2, "guest");
    expect(room.host().snapshot().seats.size() == 2, "the guest holds the Empire row");
    // The host opens its options modal now: it keeps a copy of the options, rows included.
    const auto opened = require(net::decode_galactic_lobby_game(room.host().snapshot().game));
    auto choice = room.sessions[1].snapshot().seats[1].selection;
    choice.slot = 3; choice.faction = "UNDERWORLD"; choice.team = 0;
    require(room.sessions[1].request(net::LobbyAction::select, room.revision(), choice));
    room.rounds(6);
    const auto live = require(net::decode_galactic_lobby_game(room.host().snapshot().game));
    expect(live.rows[0].human && !live.rows[1].human && live.rows[2].human, "the host's room plans the guest's new faction");
    const auto guest_view = require(net::decode_galactic_lobby_game(room.sessions[1].snapshot().game));
    expect(guest_view == live, "the guest sees the same rows");
    for (const auto& seat : room.host().snapshot().seats)
        expect(seat.selection.team == (seat.selection.slot == 1 ? 1U : 2U), "each seat is on the side of its row");
    expect(net::validate_galactic_lobby(room.host().snapshot(), false, locals[0]->catalog()).has_value(), "the live room validates");
    // The host presses OK in the modal it opened before the guest moved: its copy has the old rows (the guest's new row not planned).
    auto edited = opened;
    edited.auto_resolve = opened.auto_resolve == 1 ? 2U : 1U;
    const auto stale = room.host().configure_game(require(net::encode_galactic_lobby_game(edited)), room.host().snapshot().seats, room.revision());
    expect(!stale, "the modal's stale rows are refused (the guest's new row would be AI)");
    edited.rows = live.rows; // what the screen now does on accept: the rows come from the room as it is
    require(room.host().configure_game(require(net::encode_galactic_lobby_game(edited)), room.host().snapshot().seats, room.revision()));
    const auto after_ok = require(net::decode_galactic_lobby_game(room.host().snapshot().game));
    expect(after_ok.auto_resolve == edited.auto_resolve && after_ok.rows == live.rows,
        "OK after a guest reshape succeeds: the modal's setting is applied and the guest's row stays planned");
    const auto guest = room.host().snapshot().seats[1];
    require(room.host().assign(guest.peer, guest.selection, guest.selection.slot, room.revision()));
    room.rounds(3);
    room.propose();
    const auto frozen = room.host().snapshot();
    expect(frozen.start_attempt != net::LobbyDigest{} && require(net::decode_galactic_lobby_game(frozen.game)).rows == live.rows,
        "the concurrent preparation manifest freezes the guest's normalized free-faction choice");
    expect(!room.sessions[1].request(net::LobbyAction::select, room.revision(), before.seats[1].selection),
        "a guest cannot reshape the faction rows after the preparation manifest freezes");
    require(room.sessions[1].accept_setup());
    room.rounds(20);
    for (auto& session : room.sessions) {
        auto result = require(session.take_result());
        const auto& offer = result.setup().offer();
        expect(offer.seats.size() == 2 && offer.seats[0].player == 1 && offer.seats[1].player == 3,
            "the unanimous Start and setup retain the reshaped normalized human seats");
    }
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

void saved_mapping_clears_every_acceptance() {
    auto locals = locals_of(2);
    const auto options = two_side_options(*locals[0]);
    GcRoom room(locals, options, 1);
    room.admit(1, 2, "guest");
    room.all_ready();
    auto assignments = room.host().snapshot().seats;
    expect(std::all_of(assignments.begin(), assignments.end(), [](const auto& row) { return row.ready; }),
        "host and guest both accepted before saved mapping changes");
    const auto first = assignments[0].selection;
    assignments[0].selection = assignments[1].selection; assignments[0].selection.name = first.name;
    assignments[1].selection = first; assignments[1].selection.name = "guest";
    assignments[0].player = 2; assignments[1].player = 1;
    require(room.host().configure_game(room.host().snapshot().game, assignments, room.revision()));
    room.rounds(3);
    for (const auto& session : room.sessions)
        expect(std::none_of(session.snapshot().seats.begin(), session.snapshot().seats.end(), [](const auto& row) { return row.ready; }),
            "atomic saved faction mapping clears every prior acceptance on both peers");
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

void mounted_mod_warning() {
    namespace mods = eawr::mods;
    mods::DescriptionEntry alpha; alpha.kind = mods::DescriptionKeyKind::workshop;
    alpha.key = "workshop:alpha"; alpha.name = "Alpha"; alpha.version = "1";
    alpha.checksum = std::array<std::uint8_t, 32>{};
    auto beta = alpha; beta.key = "workshop:beta"; beta.name = "Beta";
    const mods::ModDescription host{true, {alpha, beta}};
    expect(eawr::viewer::mod_mismatch_message(host, host).empty(), "matching mods do not prompt");
    const auto missing = eawr::viewer::mod_mismatch_message(host, {true, {alpha}});
    expect(missing.find("Beta") != std::string::npos, "missing mod is named");
    const auto reordered = eawr::viewer::mod_mismatch_message(host, {true, {beta, alpha}});
    expect(reordered.find("Alpha") != std::string::npos && reordered.find("Beta") != std::string::npos,
        "both orders name their mods");
    expect(eawr::viewer::mod_mismatch_message(host, {}).empty(), "incomplete identities do not invent a mismatch");
    expect(eawr::viewer::mod_comparison_incomplete(host, {}), "unavailable identities require an uncertain review");
    expect(!eawr::viewer::mod_comparison_incomplete(host, host), "complete identical content needs no uncertain review");
    auto changed = beta; changed.version = "2";
    const auto version = eawr::viewer::mod_mismatch_message(host, {true, {alpha, changed}});
    expect(version.find("Beta") != std::string::npos && version.find("host version 1") != std::string::npos
        && version.find("your version 2") != std::string::npos, "version differences identify the host and guest");
}

// #5573: a campaign Start that the room aborts keeps the room's own reason, and the player's text names the part
// of the two setups that differs instead of blaming the online service.
void refused_start_is_diagnosed() {
    const auto refused = [](GcRoom& room) {
        room.admit(1, 2, "guest");
        room.propose();
        (void)room.sessions[1].accept_setup();
        room.rounds(30);
        expect(room.host().snapshot().phase == net::LobbyPhase::staging && room.host().notice()
            && room.host().notice()->reason == net::LobbyReason::setup_failed, "the host reopens staging with a setup_failed notice");
        return *room.host().notice();
    };
    const auto text_of = [](const net::LobbyNotice& notice) {
        const auto diagnostic = viewer::galactic_start_aborted(notice);
        expect(diagnostic.code == "EAWR-NET-1609", "a setup refusal is EAWR-NET-1609");
        return viewer::online_start_failure_text(diagnostic, viewer::LanReason::refused, true);
    };
    const std::string tail = " Both players need the same game version, mods and campaign data.";
    {
        auto locals = locals_of(2);
        locals[1]->content.ai_configuration = [](std::string_view faction, gc::PlayerId player, std::string_view) {
            std::string text = "other:" + std::string(faction) + ":" + std::to_string(player);
            return eawr::core::Result<Bytes>::success(Bytes(text.begin(), text.end()));
        };
        const auto options = two_side_options(*locals[0]);
        GcRoom room(locals, options, 1);
        const auto notice = refused(room);
        expect(notice.message == "host offer differs from frozen room choices: ai-configuration",
            "the guest names its differing AI claim: " + notice.message);
        expect(text_of(notice) == "The campaign could not be set up with the other player. The AI configuration differs between your games." + tail,
            "a differing AI claim is named in plain words");
    }
    {
        auto locals = locals_of(2);
        locals[1]->content.xml_digest = std::string(64, 'd');
        const auto options = two_side_options(*locals[0]);
        GcRoom room(locals, options, 1);
        const auto notice = refused(room);
        expect(notice.message == "match identity mismatch: gc-ai-data", "the identity refusal names its domain: " + notice.message);
        expect(text_of(notice) == "The campaign could not be set up with the other player. The AI configuration differs between your games." + tail,
            "differing AI data is named in plain words");
    }
    {
        auto locals = locals_of(2);
        locals[1]->content.script_manifest_digest = std::string(64, 'e');
        const auto options = two_side_options(*locals[0]);
        GcRoom room(locals, options, 1);
        const auto notice = refused(room);
        const auto text = text_of(notice);
        expect(text.find("online service") == std::string::npos && text.ends_with(tail), "any other refusal gets the curated sentence: " + notice.message);
    }
    // The room's own reason survives (a withdrawn guest used to read as a setup failure).
    auto locals = locals_of(2);
    const auto options = two_side_options(*locals[0]);
    GcRoom withdrawn(locals, options, 1);
    withdrawn.admit(1, 2, "guest");
    withdrawn.propose();
    require(withdrawn.sessions[1].request(net::LobbyAction::unready, withdrawn.revision()));
    withdrawn.rounds(3);
    const auto& notice = withdrawn.host().notice();
    expect(withdrawn.host().snapshot().phase == net::LobbyPhase::staging && notice && notice->reason == net::LobbyReason::not_ready,
        "a guest withdrawing during Start aborts it with not_ready");
    const auto diagnostic = viewer::galactic_start_aborted(notice);
    expect(diagnostic.code == net::lobby_diagnostic(net::LobbyReason::not_ready, "").code && diagnostic.message == notice->message,
        "the Start diagnostic keeps the notice's reason and message");
    expect(viewer::online_start_failure_text(diagnostic, viewer::LanReason::cancelled, true).find("no longer ready") != std::string::npos,
        "a withdrawn guest has its own online text");
}

// The Start log line both peers write: equal for agreeing peers, and it shows which input differs when they do not.
void start_summary_compares_peers() {
    const auto summary_of = [](Local& local, bool host) {
        viewer::GalacticLobbyBackend backend(local.content);
        const auto options = two_side_options(local);
        const auto snapshot = snapshot_of(options, {seat_of(0, 1, options, 1, "Host"), seat_of(1, 2, options, 2, "Guest")});
        const std::vector<net::SetupSeat> seats{{0, {1}, 1}, {1, {2}, 2}};
        return backend.start_summary(require(backend.offer(snapshot, seats)), host);
    };
    const auto field = [](const std::string& line, const std::string& name) {
        const auto at = line.find(" " + name + "=");
        if (at == std::string::npos) return std::string{};
        const auto start = at + name.size() + 2;
        return line.substr(start, line.find(' ', start) - start);
    };
    Local host, same, other_ai;
    other_ai.content.ai_configuration = [](std::string_view faction, gc::PlayerId player, std::string_view) {
        std::string text = "other:" + std::string(faction) + ":" + std::to_string(player);
        return eawr::core::Result<Bytes>::success(Bytes(text.begin(), text.end()));
    };
    const auto a = summary_of(host, true), b = summary_of(same, false), c = summary_of(other_ai, false);
    expect(a.starts_with("role=host ") && b.starts_with("role=guest "), "the line names the role: " + a);
    for (const char* name : {"mods", "scripts", "xml", "payload", "ai", "tick_zero", "identity", "domains"})
        expect(!field(a, name).empty() && field(a, name) == field(b, name), std::string("agreeing peers log an equal ") + name);
    expect(field(a, "payload") != field(c, "payload") && field(a, "ai") != field(c, "ai") && field(a, "domains") != field(c, "domains"),
        "a differing AI claim shows in the payload, AI and domain digests");
    expect(field(a, "xml") == field(c, "xml") && field(a, "scripts") == field(c, "scripts"), "the content digests that did not change stay equal");
    expect(a.find("gc-room") == std::string::npos && a.find("Host") == std::string::npos && a.find("Guest") == std::string::npos,
        "the line carries no room code or player name");
    expect(a.find("domains=gc-ai-controllers:") != std::string::npos || a.find("gc-ai-controllers:") != std::string::npos, "identity domains are listed by name");
}

// The message strings the setup service and the guest derivation produce, and what the player is told for each.
void setup_failure_texts() {
    const std::string generic = "The campaign could not be set up with the other player. Both players need the same game version, mods and campaign data.";
    const auto setup_failed = [](std::string message) { return net::lobby_diagnostic(net::LobbyReason::setup_failed, std::move(message)); };
    const auto text = [&](std::string message, viewer::LanReason reason = viewer::LanReason::refused) {
        return viewer::online_start_failure_text(setup_failed(std::move(message)), reason, true);
    };
    expect(text("host offer differs from frozen room choices") == generic, "an unnamed offer difference gets the generic sentence");
    expect(text("tick-zero state differs") == generic, "a tick-zero refusal gets the generic sentence");
    expect(text("Campaign content is unavailable.") == generic, "a guest derivation error gets the generic sentence");
    expect(text("cannot read C:\\Users\\someone\\data\\file.xml") == generic, "a raw implementation string is never passed on");
    expect(text("host offer differs from frozen room choices: start-state").find("The mods or campaign data differ between your games.") != std::string::npos,
        "a differing start state is named as campaign data");
    expect(text("match identity mismatch: gc-map").find("differs") == std::string::npos, "the data sentence is plural");
    expect(text("match identity mismatch: gc-rules").find("The game version differs") != std::string::npos, "differing rules read as the game version");
    expect(text("match identity mismatch: gc-map").find("The mods or campaign data differ between your games.") != std::string::npos, "a differing map reads as campaign data");
    expect(text("match identity mismatch: something-new") == generic, "an unknown identity domain gets the generic sentence");
    expect(text("setup readiness wait expired", viewer::LanReason::timeout) == "The campaign setup took too long. Try again.", "a timeout says so");
    expect(text("peer disconnected during setup", viewer::LanReason::peer_left).find("left while") != std::string::npos, "a lost peer says so");
    expect(viewer::online_start_failure_text(net::lobby_diagnostic(net::LobbyReason::link_closed, "peer lost during Start announcement"),
        viewer::LanReason::peer_left, true).find("connection to the other player was lost") != std::string::npos, "link_closed has its own text");
    expect(viewer::online_start_failure_text(net::lobby_diagnostic(net::LobbyReason::invalid_choice, "x"), viewer::LanReason::refused, true).empty(),
        "other codes keep their existing handling");
    // Online skirmish rooms fail Start with the same codes; none of the campaign wording reaches them.
    for (const auto reason : {net::LobbyReason::setup_failed, net::LobbyReason::not_ready, net::LobbyReason::link_closed,
             net::LobbyReason::left, net::LobbyReason::kicked, net::LobbyReason::host_closed})
        expect(viewer::online_start_failure_text(net::lobby_diagnostic(reason, "match identity mismatch: gc-ai-data"),
            viewer::LanReason::refused, false).empty(), "a skirmish room keeps the generic online text for every Start code");
    const auto absent = viewer::galactic_start_aborted(std::nullopt);
    expect(absent.code == "EAWR-NET-1609" && absent.message == "The campaign room ended before commitment.", "a room without a notice is a setup failure");

    expect(viewer::failure_log_detail(setup_failed("host offer differs from frozen room choices: ai-configuration"))
        == "message=\"host offer differs from frozen room choices: ai-configuration\"", "the log carries the real setup message");
    expect(viewer::failure_log_detail(setup_failed("cannot read C:\\Users\\someone\\data\\file.xml now")) == "message=\"cannot read <path> now\"",
        "path-like tokens are cut from the logged message");
    expect(viewer::failure_log_detail(setup_failed("say \"hi\"")) == "message=\"say 'hi'\"", "quotes cannot break the log line");
    expect(viewer::failure_log_detail(setup_failed(std::string(400, 'a'))).size() < 200, "the logged message is bounded");
    eawr::core::Diagnostic named; named.code = "EAWR-NET-1762"; named.message = "Player Name could not reach Other Name";
    expect(viewer::failure_log_detail(named).empty(), "messages that may name players are not logged");
    named.code = "EAWR-NET-1520";
    expect(viewer::failure_log_detail(named).empty(), "service messages are not logged");
}

// ---- the viewer's own match (what the lobby screen drives) over real loopback TCP ----

// The legacy two-build path remains an independent reference for the combined preparation.
void combined_agreement_identity() {
    Local local;
    const auto options = two_side_options(local);
    const auto snapshot = snapshot_of(options, {seat_of(0, 1, options, 1, "Host"), seat_of(1, 2, options, 2, "Guest")});
    const auto draft = require(net::galactic_roster_draft(snapshot, cp::list_mp_lobby_campaigns(local.data)));
    const cp::MpSessionTerms terms{options.seed, local.content.command_quota, local.content.byte_quota};
    const auto sealed = require(cp::seal_mp_roster(local.content, draft, terms));
    auto reference = require(cp::derive_mp_agreement(local.content, sealed, terms));
    std::vector<std::string> hashes;
    const eawr::platform::ThreadWorkerAdapter reference_workers(1);
    for (unsigned tick = 0; tick < 12; ++tick) hashes.push_back(require(reference.session->step(reference_workers)).state_hash.get());
    const auto replay = require(gc::write_replay(reference.session->record()));
    for (const unsigned workers : {1u, 2u, 4u, 8u}) {
        auto candidate = draft;
        auto combined = require(cp::seal_mp_agreement(local.content, candidate, terms));
        expect(candidate == sealed && combined.content == reference.content && combined.tick_zero == reference.tick_zero
            && combined.ai_configuration == reference.ai_configuration, "combined preparation retains every legacy claim and identity domain");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (const auto& hash : hashes) expect(require(combined.session->step(executor)).state_hash.get() == hash,
            "combined preparation preserves every legacy tick on 1/2/4/8 workers");
        expect(require(gc::write_replay(combined.session->record())) == replay, "combined preparation preserves legacy replay bytes");
    }
    auto unchanged_draft = draft;
    auto bad_terms = terms; ++bad_terms.command_quota;
    expect(!cp::seal_mp_agreement(local.content, unchanged_draft, bad_terms) && unchanged_draft == draft,
        "failed preparation retains refusal and leaves the caller's draft unchanged");
}

void preparation_attempt_lifetime() {
    Local local;
    unsigned preparations = 0;
    local.content.prepare_session = [&](const cp::CampaignSetupPlan& plan, std::string_view)
        -> eawr::core::Result<cp::MpPreparedSession> {
        ++preparations;
        return eawr::core::Result<cp::MpPreparedSession>::success({
            std::make_shared<cp::GameSession>(require(cp::GameSession::create(plan.galaxy()))), {}});
    };
    viewer::GalacticLobbyBackend backend(local.content);
    const auto options = two_side_options(local);
    auto snapshot = snapshot_of(options, {seat_of(0, 1, options, 1, "Host"), seat_of(1, 2, options, 2, "Guest")});
    const std::vector<net::SetupSeat> seats{{0, {1}, 1}, {1, {2}, 2}};
    auto offer = require(backend.offer(snapshot, seats));
    require(backend.offer(snapshot, seats));
    const auto local_identity = require(backend.identify(offer));
    const auto owner = backend.agreement();
    expect(preparations == 1, "one preparation covers sealing, repeated offer and identify in one attempt");
    auto claimed = offer; claimed.identity.build = "foreign build";
    expect(require(backend.identify(claimed)).content == local_identity.content && preparations == 1,
        "cached identity remains local even when the remote identity claim differs");
    auto changed_payload = require(net::decode_galactic_setup(offer.setup));
    changed_payload.roster.tick_zero[0] ^= 1;
    claimed = offer; claimed.setup = require(net::encode_galactic_setup(changed_payload));
    expect(require(backend.identify(claimed)).content != local_identity.content && preparations == 2,
        "changed tick-zero claim independently derives a differing identity");
    ++snapshot.revision;
    offer = require(backend.offer(snapshot, seats));
    require(backend.identify(offer));
    expect(preparations == 3 && backend.agreement()->session != owner->session, "changed revision creates a fresh owner");
    snapshot.room += "-next";
    offer = require(backend.offer(snapshot, seats));
    require(backend.identify(offer));
    expect(preparations == 4, "identical choices in a different room cannot reuse an owner");
    const eawr::platform::ThreadWorkerAdapter executor(1);
    require(backend.agreement()->session->step(executor));
    require(backend.identify(offer));
    expect(preparations == 5 && backend.agreement()->session->completed_tick() == 0,
        "an advanced candidate is independently rebuilt at tick zero");
    offer = require(backend.offer(snapshot, seats));
    require(backend.identify(offer));
    expect(preparations == 6, "discarded advanced preparation cannot reenter through offer");
    backend.set_adapter_factory([](const net::SetupOffer&, const cp::MpAgreement&)
        -> eawr::core::Result<std::unique_ptr<net::SessionAdapter>> {
        return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::failure(
            net::lobby_diagnostic(net::LobbyReason::setup_failed, "intentional factory failure"));
    });
    expect(!backend.build(offer), "factory failure is reported");
    require(backend.offer(snapshot, seats));
    expect(preparations == 7, "handover invalidates preparation even when the factory fails");
    auto bad_ai = require(net::decode_galactic_setup(offer.setup));
    auto ai = std::find_if(bad_ai.roster.players.begin(), bad_ai.roster.players.end(), [](const auto& player) {
        return !player.ai_configuration.empty();
    });
    expect(ai != bad_ai.roster.players.end(), "fixture includes an AI claim");
    ai->ai_configuration.push_back(42);
    claimed = offer; claimed.setup = require(net::encode_galactic_setup(bad_ai));
    expect(require(backend.identify(claimed)).content != local_identity.content,
        "changed AI claim independently derives a differing identity");
}

// #6045: the derivation runs on a worker thread and the caller keeps polling. Pending while it runs, one derivation for equal
// inputs (a different revision or readiness does not restart it), another request waits for it, and the result is the one a
// synchronous offer() would have cached.
void asynchronous_offer() {
    Local local;
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool open = false;
    std::atomic<unsigned> preparations{};
    std::atomic<bool> off_thread{};
    const auto caller = std::this_thread::get_id();
    local.content.prepare_session = [&](const cp::CampaignSetupPlan& plan, std::string_view)
        -> eawr::core::Result<cp::MpPreparedSession> {
        ++preparations;
        if (std::this_thread::get_id() != caller) off_thread = true;
        std::unique_lock lock(gate_mutex);
        gate_changed.wait(lock, [&] { return open; });
        return eawr::core::Result<cp::MpPreparedSession>::success({
            std::make_shared<cp::GameSession>(require(cp::GameSession::create(plan.galaxy()))), {}});
    };
    auto backend = std::make_shared<viewer::GalacticLobbyBackend>(local.content);
    auto options = two_side_options(local);
    auto snapshot = snapshot_of(options, {seat_of(0, 1, options, 1, "Host"), seat_of(1, 2, options, 2, "Guest")});
    const std::vector<net::SetupSeat> seats{{0, {1}, 1}, {1, {2}, 2}};
    const auto pending = [](const eawr::core::Result<net::SetupOffer>& answer) { return !answer && net::is_start_pending(answer.error()); };
    const auto wait_for = [](const auto& condition) {
        for (unsigned i = 0; i < 3000 && !condition(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return condition();
    };

    expect(viewer::GalacticLobbyBackend::PrepareProgress{}.state == viewer::GalacticLobbyBackend::PrepareProgress::State::idle
        && backend->prepare_progress().state == viewer::GalacticLobbyBackend::PrepareProgress::State::idle,
        "#6045 nothing is being prepared before the first request");
    expect(pending(viewer::GalacticLobbyBackend::offer_async(backend, snapshot, seats)), "#6045 the first request answers pending at once");
    expect(wait_for([&] { return preparations.load() == 1; }), "#6045 the worker started the derivation");
    expect(off_thread.load(), "#6045 the derivation does not run on the calling thread");
    expect(backend->prepare_progress().state == viewer::GalacticLobbyBackend::PrepareProgress::State::running,
        "#6045 progress reads running while the derivation runs");

    // The room moved on (revision, readiness) without changing what is derived: the running derivation serves it.
    auto later = snapshot; later.revision += 3; later.frozen_revision = later.revision;
    for (auto& seat : later.seats) seat.ready = !seat.ready;
    for (unsigned i = 0; i < 5; ++i)
        expect(pending(viewer::GalacticLobbyBackend::offer_async(backend, later, seats)), "#6045 an equal request stays pending");
    expect(preparations.load() == 1, "#6045 equal inputs never start a second derivation");

    // Different inputs (another seed) wait for the running derivation to end and say so.
    auto other_options = options; other_options.seed += 7;
    const auto other = snapshot_of(other_options, {seat_of(0, 1, other_options, 1, "Host"), seat_of(1, 2, other_options, 2, "Guest")});
    expect(pending(viewer::GalacticLobbyBackend::offer_async(backend, other, seats)), "#6045 a different request answers pending");
    expect(backend->prepare_progress().state == viewer::GalacticLobbyBackend::PrepareProgress::State::waiting_for_earlier
        && preparations.load() == 1, "#6045 a different request waits for the earlier derivation and says so, no second derivation runs");

    // A second backend in the same process waits too: one derivation at a time.
    auto sibling = std::make_shared<viewer::GalacticLobbyBackend>(local.content);
    expect(pending(viewer::GalacticLobbyBackend::offer_async(sibling, snapshot, seats))
        && sibling->prepare_progress().state == viewer::GalacticLobbyBackend::PrepareProgress::State::waiting_for_earlier
        && preparations.load() == 1, "#6045 another backend waits for the running derivation");
    sibling->abandon_prepare();

    { const std::lock_guard lock(gate_mutex); open = true; }
    gate_changed.notify_all();
    // Back to the first request: it gets the result of the derivation that ran, once.
    auto answered = viewer::GalacticLobbyBackend::offer_async(backend, later, seats);
    expect(wait_for([&] { answered = viewer::GalacticLobbyBackend::offer_async(backend, later, seats); return !pending(answered); }),
        "#6045 the finished derivation answers");
    const auto offer = require(std::move(answered));
    expect(preparations.load() == 1, "#6045 the answer is the one derivation, not a new one");
    expect(backend->prepare_progress().state == viewer::GalacticLobbyBackend::PrepareProgress::State::idle,
        "#6045 progress reads idle once the answer is taken");
    require(backend->identify(offer));
    expect(preparations.load() == 1 && backend->agreement(), "#6045 identify reuses the asynchronous preparation");
    const auto repeated = viewer::GalacticLobbyBackend::offer_async(backend, later, seats);
    expect(repeated && repeated.value().setup == offer.setup, "#6045 a repeated request is answered from the cache");

    // The different request now gets its own derivation (the earlier one ended, its result was for other inputs).
    expect(wait_for([&] { return !pending(viewer::GalacticLobbyBackend::offer_async(backend, other, seats)); }),
        "#6045 the different request is derived after the earlier one ended");
    expect(preparations.load() == 2, "#6045 exactly one more derivation ran for the different inputs");
    backend->release_prepare();
    sibling->release_prepare();
}

// #6117 review: the previous preparation (a whole prepared campaign session) is freed before the next derivation starts, so a retry
// after an aborted attempt never holds two sessions at the derivation's peak.
void asynchronous_offer_frees_the_previous_preparation() {
    Local local;
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool second_may_finish = false;
    std::mutex sessions_mutex;
    std::vector<std::weak_ptr<cp::GameSession>> sessions;
    local.content.prepare_session = [&](const cp::CampaignSetupPlan& plan, std::string_view)
        -> eawr::core::Result<cp::MpPreparedSession> {
        auto session = std::make_shared<cp::GameSession>(require(cp::GameSession::create(plan.galaxy())));
        std::size_t index{};
        { const std::lock_guard lock(sessions_mutex); sessions.push_back(session); index = sessions.size(); }
        if (index > 1) {
            std::unique_lock lock(gate_mutex);
            gate_changed.wait(lock, [&] { return second_may_finish; });
        }
        return eawr::core::Result<cp::MpPreparedSession>::success({std::move(session), {}});
    };
    auto backend = std::make_shared<viewer::GalacticLobbyBackend>(local.content);
    const auto options = two_side_options(local);
    const auto first = snapshot_of(options, {seat_of(0, 1, options, 1, "Host"), seat_of(1, 2, options, 2, "Guest")});
    const std::vector<net::SetupSeat> seats{{0, {1}, 1}, {1, {2}, 2}};
    const auto wait_for = [](const auto& condition) {
        for (unsigned i = 0; i < 3000 && !condition(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return condition();
    };
    eawr::core::Result<net::SetupOffer> answer = viewer::GalacticLobbyBackend::offer_async(backend, first, seats);
    expect(wait_for([&] {
        answer = viewer::GalacticLobbyBackend::offer_async(backend, first, seats);
        return answer || !net::is_start_pending(answer.error());
    }) && answer, "#6045 the first derivation answers");
    std::weak_ptr<cp::GameSession> first_session;
    { const std::lock_guard lock(sessions_mutex); expect(sessions.size() == 1, "one session was built"); first_session = sessions.front(); }
    expect(!first_session.expired(), "#6045 the answered preparation holds its session for the identify that follows");
    // A retry with other inputs (the room changed after an aborted attempt): the old session is gone before the new derivation finishes.
    auto changed = options; changed.seed += 11;
    const auto second = snapshot_of(changed, {seat_of(0, 1, changed, 1, "Host"), seat_of(1, 2, changed, 2, "Guest")});
    const auto pending = viewer::GalacticLobbyBackend::offer_async(backend, second, seats);
    expect(!pending && net::is_start_pending(pending.error()), "#6045 the retry derives again");
    expect(first_session.expired(), "#6045 the previous preparation's session is freed before the new derivation runs");
    { const std::lock_guard lock(gate_mutex); second_may_finish = true; }
    gate_changed.notify_all();
    expect(wait_for([&] {
        const auto result = viewer::GalacticLobbyBackend::offer_async(backend, second, seats);
        return result || !net::is_start_pending(result.error());
    }), "#6045 the retry's derivation finishes");
    backend->release_prepare();
}

// #6045: a failing derivation answers its failure once, then a retry derives again.
void asynchronous_offer_failure() {
    Local local;
    std::atomic<unsigned> attempts{};
    local.content.prepare_session = [&](const cp::CampaignSetupPlan& plan, std::string_view)
        -> eawr::core::Result<cp::MpPreparedSession> {
        if (++attempts == 1) return eawr::core::Result<cp::MpPreparedSession>::failure(
            net::lobby_diagnostic(net::LobbyReason::setup_failed, "synthetic derivation failure"));
        return eawr::core::Result<cp::MpPreparedSession>::success({
            std::make_shared<cp::GameSession>(require(cp::GameSession::create(plan.galaxy()))), {}});
    };
    auto backend = std::make_shared<viewer::GalacticLobbyBackend>(local.content);
    const auto options = two_side_options(local);
    const auto snapshot = snapshot_of(options, {seat_of(0, 1, options, 1, "Host"), seat_of(1, 2, options, 2, "Guest")});
    const std::vector<net::SetupSeat> seats{{0, {1}, 1}, {1, {2}, 2}};
    std::optional<eawr::core::Result<net::SetupOffer>> answer;
    for (unsigned i = 0; i < 3000; ++i) {
        answer = viewer::GalacticLobbyBackend::offer_async(backend, snapshot, seats);
        if (!(!*answer && net::is_start_pending(answer->error()))) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(answer && !*answer && !net::is_start_pending(answer->error())
        && answer->error().message.find("synthetic derivation failure") != std::string::npos,
        "#6045 a failed derivation answers its own failure, not pending");
    for (unsigned i = 0; i < 3000; ++i) {
        answer = viewer::GalacticLobbyBackend::offer_async(backend, snapshot, seats);
        if (!(!*answer && net::is_start_pending(answer->error()))) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(answer && *answer && attempts.load() == 2, "#6045 a retry after the failure derives again and succeeds");
    backend->release_prepare();
}

void wire_identity_and_preparation_keys() {
    Local local;
    unsigned preparations = 0;
    local.content.prepare_session = [&](const cp::CampaignSetupPlan& plan, std::string_view)
        -> eawr::core::Result<cp::MpPreparedSession> {
        ++preparations;
        return eawr::core::Result<cp::MpPreparedSession>::success({
            std::make_shared<cp::GameSession>(require(cp::GameSession::create(plan.galaxy()))), {}});
    };
    viewer::GalacticLobbyBackend backend(local.content);
    const auto options = two_side_options(local);
    const auto snapshot = snapshot_of(options, {seat_of(0, 1, options, 1, "Host"), seat_of(1, 2, options, 2, "Guest")});
    const std::vector<net::SetupSeat> seats{{0, {1}, 1}, {1, {2}, 2}};
    const auto offer = require(backend.offer(snapshot, seats));
    const auto cached_identity = require(backend.identify(offer));
    const auto cached = backend.agreement();
    expect(preparations == 1, "wire identity fixture has one cached preparation");
    viewer::GalacticLobbyBackend fresh(local.content);
    const auto decoded_identity = require(fresh.identify(offer));
    expect(preparations == 2 && decoded_identity.build == cached_identity.build
        && decoded_identity.content == cached_identity.content && fresh.agreement()->tick_zero == cached->tick_zero,
        "fresh identify of the decoded wire roster agrees with the cached sealed preparation");

    const auto misses = [&](const auto& change, const char* message) {
        auto changed = offer;
        change(changed);
        const auto before = preparations;
        require(backend.identify(changed));
        expect(preparations == before + 1 && backend.agreement()->session != cached->session, message);
        const auto after_miss = preparations;
        require(backend.identify(offer));
        expect(preparations == after_miss && backend.agreement()->session == cached->session,
            "only the exact original inputs recover the unchanged candidate");
    };
    misses([](auto& value) { ++value.seed; }, "changed seed independently derives");
    misses([](auto& value) { ++value.seats.front().peer; }, "changed seat mapping independently derives");
    misses([](auto& value) { --value.final_tick_count; }, "changed final tick independently derives");
    misses([](auto& value) { value.recovery.reconnect = !value.recovery.reconnect; }, "changed recovery independently derives");
    misses([](auto& value) { ++value.input_delay; }, "changed input delay independently derives");
    misses([](auto& value) { value.domain += "-other"; }, "changed domain independently derives");

    const auto payload = require(net::decode_galactic_setup(offer.setup));
    // GCSU v3: magic/version, two length-prefixed texts, custom, three i64 options, heroes, the u32 vote timeout, then two u32 quotas.
    const auto quota_offset = 8U + 4U + payload.roster.campaign.size() + 4U + payload.roster.difficulty.size() + 1U + 24U + 1U + 4U;
    for (const auto offset : {quota_offset, quota_offset + 4U}) {
        auto changed = offer;
        changed.setup.at(offset) ^= 1U; // Invalid quotas cannot be produced by the canonical encoder.
        const auto before = preparations;
        const auto agreed = backend.agreement();
        expect(!backend.identify(changed) && preparations == before && backend.agreement() == agreed,
            "identify refuses a changed wire quota before reuse or derivation and preserves the agreement");
    }
}

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

// #4554: the scripted GC LAN acceptance (--eawr-gc-lan-final-tick) shortens host and joiner alike. The backend exposes
// the value so the lobby's setup hook can apply it to the session limit that setup.cpp checks against the offer;
// a real game never sets it and keeps the default cap.
void preparation_content_manifest() {
    const auto root = std::filesystem::temp_directory_path() / ("eawr-prepare-content-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path root; ~Cleanup() { eawr::testing::remove_temp_directory(root); } } cleanup{root};
    for (const auto* folder : {"base/xml", "base/scripts", "base/custom", "leaf/scripts", "parent/scripts"}) std::filesystem::create_directories(root / folder);
    const auto write = [&](const auto& path, std::string_view bytes) {
        std::ofstream output(root / path); output << bytes;
        if (!output) throw std::runtime_error("could not write preparation content fixture");
    };
    write("base/xml/factionfiles.xml", "<Faction_Files><File>factions.xml</File></Faction_Files>");
    write("base/xml/factions.xml", "<Factions><Faction Name=\"SYNTHETIC\"/></Factions>");
    write("base/xml/traderoutefiles.xml", "<Trade_Route_Files><File>data/custom/routes.xml</File></Trade_Route_Files>");
    write("base/custom/routes.xml", "<Trade_Routes/>");
    write("base/xml/gameconstants.xml", "<Game_Constants/>");
    write("base/scripts/test.lua", "return 1");
    write("leaf/scripts/leaf.lua", "return 3");
    write("parent/scripts/parent.lua", "return 4");
    std::filesystem::create_directories(root / "relocated");
    for (const auto* folder : {"base", "leaf", "parent"})
        std::filesystem::copy(root / folder, root / "relocated" / folder, std::filesystem::copy_options::recursive);
    const auto relocated_xml = root / "relocated/base/xml/factions.xml";
    std::filesystem::last_write_time(relocated_xml, std::filesystem::last_write_time(relocated_xml) + std::chrono::hours(1));
    // Per-install caches are outside the mounted data roots and never enter the content commitment.
    std::filesystem::create_directories(root / "shader-cache");
    std::filesystem::create_directories(root / "model-cache");
    write("shader-cache/cache.bin", "local shader cache");
    write("model-cache/cache.bin", "local model cache");
    const auto load = [&](bool reverse, bool aliases = false, bool relocated = false) {
        const auto mounted = relocated ? root / "relocated" : root;
        std::vector<eawr::vfs::MountSpec> mounts{{aliases ? "workshop:123" : "leaf", mounted / "leaf", "data", {}},
            {aliases ? "path:manual-parent" : "parent", mounted / "parent", "data", {}}, {"base", mounted / "base", "data", {}}};
        if (reverse) std::swap(mounts[0], mounts[1]);
        auto files = std::make_shared<const eawr::vfs::Vfs>(require(eawr::vfs::Vfs::mount(mounts)));
        const auto catalog = require(eawr::data::load_catalog(*files, eawr::data::Profile::foc));
        std::vector<eawr::units::InputFile> receipts;
        const auto legacy = require(eawr::skirmish::read_data_identity(*files));
        // Pre-receipt encoding at 8e18b90e9100309ed4a401dbf1cb323d29af887b, independently derived from these
        // synthetic bytes. Comparing the two current overloads alone would miss a shared encoding regression.
        expect(legacy.script_manifest_digest == "e1ed62965b438d0f1358926dbfc905b489482af3419c64ecdbec6f731df3c56e"
            && legacy.ai_data_digest == "5d8f3c58a624c5a021eee7f6d5889e42e492f61ffbf9c1d5284c82710fc188e0",
            "the original overload preserves its pre-receipt Lua and XML identity bytes");
        const auto collected = require(eawr::skirmish::read_data_identity(*files, &receipts));
        expect(legacy.script_manifest_digest == collected.script_manifest_digest && legacy.ai_data_digest == collected.ai_data_digest
            && !receipts.empty(), "consumed-byte receipts preserve existing XML/Lua identity bytes");
        return require(viewer::load_galactic_lobby_content(*files, catalog.catalog, files));
    };
    auto first = load(false), second = load(true);
    expect(first.local.xml_digest == second.local.xml_digest
        && first.local.script_manifest_digest == second.local.script_manifest_digest
        && first.local.mounted_chain_digest != second.local.mounted_chain_digest,
        "mount order is explicit even when effective XML/Lua bytes happen to match");
    const auto renamed = load(false, true), moved = load(false, true, true);
    expect(first.local.mounted_chain_digest == renamed.local.mounted_chain_digest
        && first.local.mounted_chain_digest == moved.local.mounted_chain_digest,
        "identical mounted content agrees across workshop/manual keys, installation locations and file timestamps");
    require(first.local.verify_preparation_content());
    write("base/scripts/test.lua", "return 2");
    expect(!first.local.verify_preparation_content(), "a changed Lua module invalidates the verified preparation inputs");
    write("base/scripts/test.lua", "return 1"); require(first.local.verify_preparation_content());
    write("base/custom/routes.xml", "<Trade_Routes><!--same world, changed consumed input--></Trade_Routes>");
    expect(!first.local.verify_preparation_content(), "world registry includes outside data/xml are reverified");
    expect(load(false).local.mounted_chain_digest != first.local.mounted_chain_digest,
        "per-layer checksums cover actual world inputs outside the conventional closure");
    write("base/custom/routes.xml", "<Trade_Routes/>"); require(first.local.verify_preparation_content());
    write("base/xml/factions.xml", "<Factions><Faction Name=\"CHANGED\"/></Factions>");
    expect(!first.local.verify_preparation_content(), "changed XML invalidates earlier loaded catalog inputs");
}

void late_preparation_content_reads() {
    const auto root = std::filesystem::temp_directory_path() / ("eawr-late-content-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path root; ~Cleanup() { eawr::testing::remove_temp_directory(root); } } cleanup{root};
    std::filesystem::create_directories(root / "scripts"); std::filesystem::create_directories(root / "custom");
    const auto write = [&](std::string_view path, std::string_view bytes) {
        std::ofstream output(root / path); output << bytes;
        if (!output) throw std::runtime_error("cannot write late-read fixture");
    };
    write("scripts/lazy.lua", "return 1");
    write("custom/late-story.txt", "unagreed story input");
    write("custom/unused-hint.bin", "speculative bytes");
    write("custom/consumed.bin", "prepared bytes");
    std::filesystem::create_directories(root / "art/models");
    // The shared decoder's minimal synthetic model fixture: no retail bytes or geometry.
    const std::array<std::uint32_t, 11> words{
        0x200, 0x8000000cU, 0x201, 4, 0, 0x600, 0x80000014U, 0x601, 12, 0x00000401, 0x04040000};
    std::vector<std::byte> model_bytes;
    for (const auto word : words) for (unsigned shift = 0; shift < 32; shift += 8)
        model_bytes.push_back(static_cast<std::byte>((word >> shift) & 0xffU));
    model_bytes.insert(model_bytes.end(), 4, std::byte{});
    {
        std::ofstream output(root / "art/models/unlisted.alo", std::ios::binary);
        output.write(reinterpret_cast<const char*>(model_bytes.data()), static_cast<std::streamsize>(model_bytes.size()));
        if (!output) throw std::runtime_error("cannot write cached-model fixture");
    }
    const std::array mounts{eawr::vfs::MountSpec{"fixture", root, "data", {}}};
    auto original = std::make_shared<const eawr::vfs::Vfs>(require(eawr::vfs::Vfs::mount(mounts)));
    eawr::scene::VfsAssetCache cached(*original);
    const auto* unlisted_model = cached.model("data/art/models/unlisted.alo");
    expect(unlisted_model != nullptr, "unlisted model is already decoded in the shared presentation cache");
    std::vector<eawr::units::InputFile> scripts;
    require(eawr::skirmish::read_data_identity(*original, &scripts));
    std::vector<eawr::core::ContentInputReceipt> known;
    for (const auto& script : scripts) known.push_back({script.logical_path, script.layer_id, script.sha256});
    auto view = require(eawr::vfs::Vfs::preparation_view(original, known));
    expect(!original->has_content_contract() && view.has_content_contract(),
        "only the private new-MP preparation view enables the content contract");
    require(view.open("data/custom/consumed.bin"));
    require(view.open_for_prefetch("data/custom/unused-hint.bin"));
    const auto closure = require(view.seal_content());
    expect(closure.size() == known.size() + 1, "only consumed preparation inputs enter the sealed closure");
    require(view.open("data/scripts/lazy.lua")); // The module may first execute after Start; its bytes were agreed.
    const auto late = view.open("data/custom/late-story.txt");
    expect(!late && late.error().code == eawr::vfs::diagnostic_codes::content_unagreed,
        "late story input outside the Start-time closure is explicitly refused before bytes escape");
    require(original->open("data/custom/late-story.txt"));
    eawr::scene::VfsAssetCache checked_cache(view);
    expect(checked_cache.model("data/art/models/unlisted.alo") == nullptr
        && cached.model("data/art/models/unlisted.alo") == unlisted_model,
        "a previously decoded unlisted model cannot bypass the sealed view through the shared cache");
    expect(!view.stat("data/custom/late-story.txt") && !view.candidates("data/custom/late-story.txt")
        && !view.open_for_prefetch("data/custom/unused-hint.bin"), "late metadata and prefetch cannot bypass the agreed set");
    const auto listed = require(view.enumerate("data/custom/"));
    expect(listed.size() == 1 && listed.front().canonical_path == "data/custom/consumed.bin",
        "sealed enumeration excludes unagreed inputs while the original presentation view remains unrestricted");
    expect(require(original->enumerate("data/custom/")).size() == 3
        && require(original->open_for_prefetch("data/custom/unused-hint.bin"))
            == require(original->open("data/custom/unused-hint.bin")),
        "the shared unrestricted VFS keeps its original enumeration and decode-ahead access after MP seals");
    const auto candidate = require(original->stat("data/custom/late-story.txt"));
    std::array<std::byte, 3> buffer{};
    unsigned delivered{};
    expect(!view.open(candidate) && !view.open_prefix(candidate.canonical_path, 3)
        && !view.read_chunks(candidate, buffer, [&](auto) { ++delivered; }) && delivered == 0,
        "exact-candidate, prefix and chunk reads expose no unagreed bytes");
    const auto raw_bytes = require(original->open(candidate));
    expect(require(original->open_prefix(candidate.canonical_path, 3))
        == std::vector<std::byte>(raw_bytes.begin(), raw_bytes.begin() + 3),
        "unrestricted SP/skirmish/checkpoint/presentation reads keep the existing prefix behavior");
    std::vector<std::byte> raw_chunks;
    require(original->read_chunks(candidate, buffer, [&](const auto bytes) {
        raw_chunks.insert(raw_chunks.end(), bytes.begin(), bytes.end());
    }));
    expect(raw_chunks == raw_bytes,
        "unrestricted streaming still reads an input excluded from the separate MP closure");
    write("scripts/lazy.lua", "return 2"); // Same prefix/length: only checking a requested prefix would miss this.
    const auto script = require(original->stat("data/scripts/lazy.lua"));
    expect(original->read_location(script).has_value() && !view.read_location(script),
        "the checked view cannot expose a native-file bypass even for an agreed path");
    expect(!view.open(script) && !view.open_prefix(script.canonical_path, 3)
        && !view.read_chunks(script, buffer, [&](auto) { ++delivered; }) && delivered == 0,
        "every late-read API verifies the complete agreed bytes before delivery, including unchanged prefixes");
    const auto changed_bytes = require(original->open(script));
    expect(changed_bytes.size() == 8 && changed_bytes.back() == static_cast<std::byte>('2'),
        "unrestricted reads still expose current bytes rather than applying the MP agreement check");
    write("scripts/lazy.lua", "return 1"); require(view.open(script));
    auto failed = require(eawr::vfs::Vfs::preparation_view(original, known));
    write("scripts/lazy.lua", "return 2"); expect(!failed.open(script), "changed preparation input is refused");
    write("scripts/lazy.lua", "return 1");
    expect(!failed.seal_content(), "an ignored content verification failure cannot publish a prepared claim");
}

void cached_model_content_refresh() {
    const auto root = std::filesystem::temp_directory_path() / ("eawr-mp-model-refresh-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path root; ~Cleanup() { eawr::testing::remove_temp_directory(root); } } cleanup{root};
    std::filesystem::create_directories(root / "art/models");
    // Empty skeleton/connections with an ignored extension mini-field: both same-size variants are valid models.
    const std::array<std::uint32_t, 11> words{
        0x200, 0x8000000cU, 0x201, 4, 0, 0x600, 0x8000001aU, 0x601, 18, 0x00000401, 0x04040000};
    std::vector<std::byte> bytes;
    for (const auto word : words) for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::byte>((word >> shift) & 0xffU));
    bytes.insert(bytes.end(), 4, std::byte{});
    bytes.push_back(std::byte{0x7f}); bytes.push_back(std::byte{4}); bytes.insert(bytes.end(), 4, std::byte{});
    const auto write = [&](std::string_view name, const auto& data) {
        std::ofstream file(root / "art/models" / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file) throw std::runtime_error("cannot write MP cached-model fixture");
    };
    write("refresh.alo", bytes); write("unused.alo", bytes);
    const std::vector<std::byte> empty; write("invalid.alo", empty);
    const std::array mounts{eawr::vfs::MountSpec{"fixture", root, "data", {}}};
    auto source = std::make_shared<const eawr::vfs::Vfs>(require(eawr::vfs::Vfs::mount(mounts)));
    eawr::scene::VfsAssetCache raw(*source);
    raw.end_model_load(std::numeric_limits<std::uint64_t>::max());
    eawr::core::load_profile::Recording ordinary;
    eawr::core::load_profile::active = &ordinary;
    const auto* old = raw.model("data/art/models/refresh.alo");
    eawr::core::load_profile::active = nullptr;
    expect(old && ordinary.counters[static_cast<std::size_t>(eawr::core::load_profile::Counter::sha_reused_bytes)] == 0,
        "unrestricted gameplay keeps its original no-hash decode behavior");
    const auto initial_decodes = raw.model_stats().unique_loads;
    bytes.back() = std::byte{1}; write("refresh.alo", bytes);
    const auto expected = eawr::core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    auto view = require(eawr::vfs::Vfs::preparation_view(source, {}));
    eawr::scene::VfsAssetCache first(view), second(view);
    auto unproven_ahead = require(first.shared_model_ahead("data/art/models/refresh.alo"));
    expect(unproven_ahead.get() == old && view.model_refresh_cost().entries == 0,
        "an unused hint does not refresh or publish an unproven cached model");
    auto unused = require(first.shared_model_ahead("data/art/models/unused.alo"));
    expect(unused != nullptr, "the unused speculative model was decoded successfully");
    std::promise<void> start; const auto ready = start.get_future().share();
    auto a = std::async(std::launch::async, [&] { ready.wait(); return first.shared_model("data/art/models/refresh.alo"); });
    auto b = std::async(std::launch::async, [&] { ready.wait(); return second.shared_model("data/art/models/refresh.alo"); });
    start.set_value();
    auto refreshed = require(a.get()), same = require(b.get());
    expect(refreshed.get() == same.get() && refreshed.get() != old && old->bones.empty(),
        "racing MP consumers share one fresh decode while the old raw owner remains valid");
    expect(raw.model_stats().unique_loads == initial_decodes + 2 && view.model_refresh_cost().entries == 1,
        "one unused decode-ahead and exactly one required refresh were decoded");
    eawr::core::load_profile::Recording memo;
    eawr::core::load_profile::active = &memo;
    const auto digest = first.sha256("data/art/models/refresh.alo");
    const auto other_digest = second.sha256("data/art/models/refresh.alo");
    eawr::core::load_profile::active = nullptr;
    expect(digest == expected && other_digest == expected
        && memo.counters[static_cast<std::size_t>(eawr::core::load_profile::Counter::sha_bytes)] == 0,
        "the consumed fresh decode supplies its SHA to both getters without rereading current bytes");
    expect(!first.shared_model_ahead("data/art/models/invalid.alo") && !first.shared_model("data/art/models/invalid.alo"),
        "parse refusal is stable through the ordered consumer");
    const auto closure = require(view.seal_content());
    expect(closure.size() == 2 && std::none_of(closure.begin(), closure.end(), [](const auto& input) {
        return input.logical_path.find("unused") != std::string::npos;
    }), "only consumed model/refusal bytes enter the closure; unused successful hints do not");
    expect(!first.shared_model("data/art/models/unused.alo"), "an unused decoded hint cannot bypass the sealed agreement");
    bytes.back() = std::byte{2}; write("refresh.alo", bytes);
    expect(first.sha256("data/art/models/refresh.alo") == expected && !view.open("data/art/models/refresh.alo"),
        "cached immutable model bytes keep their own identity while a changed late file read is refused");
    require(source->open("data/art/models/refresh.alo"));

    // #6288 review: a verified view's refused read stays out of the store the unrestricted presentation cache shares.
    write("view_failure.alo", bytes);
    auto failing_source = std::make_shared<const eawr::vfs::Vfs>(require(eawr::vfs::Vfs::mount(mounts)));
    auto failing_view = require(eawr::vfs::Vfs::preparation_view(failing_source, {}));
    require(failing_view.open("data/art/models/view_failure.alo"));
    static_cast<void>(require(failing_view.seal_content()));
    bytes.back() = std::byte{3}; write("view_failure.alo", bytes);
    eawr::scene::VfsAssetCache failing_viewer(failing_view), presentation(*failing_source);
    expect(!failing_viewer.shared_model("data/art/models/view_failure.alo"),
        "an agreed file that changed after sealing is refused through the verified view");
    expect(static_cast<bool>(presentation.shared_model("data/art/models/view_failure.alo")),
        "the unrestricted presentation cache is not served the view's cached refusal");

    // A concurrent service consumer must not spend the ordered provider's incoming-entry protection on a refresh.
    auto protected_source = std::make_shared<const eawr::vfs::Vfs>(require(eawr::vfs::Vfs::mount(mounts)));
    eawr::scene::VfsAssetCache incoming(*protected_source);
    incoming.end_model_load(std::numeric_limits<std::uint64_t>::max());
    expect(incoming.model("data/art/models/refresh.alo") != nullptr, "incoming gameplay model has an unproven decode");
    incoming.begin_galactic_preparation();
    auto protected_view = require(eawr::vfs::Vfs::preparation_view(protected_source, {}));
    eawr::scene::VfsAssetCache::ModelPreparationScope preparation(protected_view, 0);
    expect(preparation.active(), "incoming entries permit a bounded preparation window");
    eawr::scene::VfsAssetCache service(protected_view), owner(protected_view);
    auto service_model = require(service.shared_model("data/art/models/refresh.alo"));
    std::weak_ptr<const eawr::assets::Model> protected_refresh = service_model;
    service_model.reset();
    expect(!protected_refresh.expired(), "a non-owning refresh retains the ordered provider's incoming protection at zero budget");
    const auto refreshed_loads = incoming.model_stats().unique_loads;
    std::string owner_digest;
    auto consumed = require(owner.shared_model("data/art/models/refresh.alo", owner_digest, &preparation));
    expect(!owner_digest.empty() && incoming.model_stats().unique_loads == refreshed_loads,
        "the ordered owner consumes the same protected refresh without another decode");
    consumed.reset();
    expect(protected_refresh.expired(), "ordered consumption releases the transferred protection and applies the zero budget");
}

void nonce_bound_preparation() {
    const auto wait_for = [](const auto& condition) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!condition()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    };
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered{}, open{};
        std::atomic<unsigned> builds{};
        std::weak_ptr<cp::GameSession> abandoned;
    };
    auto gate = std::make_shared<Gate>();
    struct OpenOnExit {
        std::shared_ptr<Gate> gate;
        ~OpenOnExit() { { const std::lock_guard lock(gate->mutex); gate->open = true; } gate->changed.notify_all(); }
    } unblock{gate};
    auto local = std::make_shared<Local>(); auto content = local->content;
    content.prepare_session = [local, gate](const cp::CampaignSetupPlan& plan, std::string_view) -> Result<cp::MpPreparedSession> {
        static_cast<void>(local);
        const auto number = ++gate->builds;
        auto game = std::make_shared<cp::GameSession>(require(cp::GameSession::create(plan.galaxy())));
        if (number == 1) {
            std::unique_lock lock(gate->mutex); gate->abandoned = game; gate->entered = true;
            gate->changed.wait(lock, [&] { return gate->open; });
        }
        return Result<cp::MpPreparedSession>::success({game, {}, {}, sim::identity_domain("provider", "immutable fixture").digest});
    };
    auto backend = std::make_shared<viewer::GalacticLobbyBackend>(content);
    const auto options = two_side_options(*local);
    auto snapshot = snapshot_of(options, {seat_of(0, 1, options, 1, "Host"), seat_of(1, 2, options, 2, "Guest")});
    snapshot.start_attempt[0] = 1;
    const std::vector<net::SetupSeat> seats{{0, {1}, 1}, {1, {2}, 2}};
    const auto pending = [](const auto& result) { return !result && net::is_start_pending(result.error()); };
    expect(pending(viewer::GalacticLobbyBackend::offer_async(backend, snapshot, seats)), "first nonce-bound preparation starts asynchronously");
    expect(wait_for([&] { const std::lock_guard lock(gate->mutex); return gate->entered; }), "first attempt owns a running candidate");
    backend->abandon_prepare(); snapshot.start_attempt[0] = 2;
    expect(pending(viewer::GalacticLobbyBackend::offer_async(backend, snapshot, seats)), "retry waits for an uninterruptible old worker without reusing it");
    { const std::lock_guard lock(gate->mutex); gate->open = true; } gate->changed.notify_all();
    std::optional<net::SetupOffer> offered;
    expect(wait_for([&] {
        auto result = viewer::GalacticLobbyBackend::offer_async(backend, snapshot, seats);
        if (pending(result)) return false;
        offered = require(std::move(result)); return true;
    }), "fresh attempt completes after discarding the old result");
    expect(gate->builds == 2 && gate->abandoned.expired(), "old nonce result is freed and equal choices derive anew exactly once");
    const auto domains = require(backend->preparation_domains(*offered));
    expect(std::any_of(domains.begin(), domains.end(), [](const auto& domain) { return domain.name == "gc-prepared-owner"; })
        && std::any_of(domains.begin(), domains.end(), [](const auto& domain) { return domain.name == "gc-prepared-provider"; }),
        "prepared claim supplements all ordinary identities with owner archives and provider content");
    require(backend->identify(*offered));
    expect(gate->builds == 2, "sealing and final identify retain the exact same candidate");
    std::weak_ptr<cp::GameSession> accepted = backend->agreement()->session;
    backend->abandon_prepare();
    expect(!backend->agreement() && accepted.expired(), "cancelling an identified attempt releases its owner and services");

    Local plain;
    auto draft = require(net::galactic_roster_draft(snapshot, plain.catalog()));
    const cp::MpSessionTerms terms{options.seed, plain.content.command_quota, plain.content.byte_quota};
    auto agreement = require(cp::seal_mp_agreement(plain.content, draft, terms));
    const auto before = require(cp::prepared_mp_domains(agreement));
    agreement.provider_identity.digest[0] = 7;
    expect(require(cp::prepared_mp_domains(agreement)) != before, "provider input mismatch is visible even with an equal galaxy hash");
    agreement.provider_identity = {}; agreement.provider_identity_required = true;
    expect(!cp::prepared_mp_domains(agreement), "an opaque custom runtime without an input identity fails closed");
}

void acceptance_final_tick_agrees() {
    auto local = std::make_shared<Local>();
    viewer::GalacticLobbyBackend backend(local->content);
    auto options = two_side_options(*local);
    const auto snapshot = snapshot_of(options, {seat_of(0, 1, options, 1, "Host"), seat_of(1, 2, options, 2, "Guest")});
    const std::vector<net::SetupSeat> seats{{0, {1}, 1}, {1, {2}, 2}};
    expect(backend.final_tick_count() == sim::galactic::galactic_max_ticks, "a real game carries no acceptance tick limit");
    expect(require(backend.offer(snapshot, seats)).final_tick_count == sim::galactic::galactic_max_ticks, "the ordinary offer keeps the default cap");
    backend.acceptance_final_tick(3900);
    expect(backend.final_tick_count() == 3900, "the acceptance limit is readable by the setup hook");
    expect(require(backend.offer(snapshot, seats)).final_tick_count == 3900, "host and joiner offers carry the same acceptance limit");
}

// The same projection used by ViewerHost, compiled without Godot.
struct LaunchOptions {
    std::shared_ptr<viewer::GalacticLanMatch> lan_match;
    std::shared_ptr<viewer::GalacticLaunch> lan_start;
    std::vector<eawr::viewer::galactic::LanPlayer> lan_players;
    std::string campaign;
    std::uint32_t seat{};
};
void verify_launch(const std::vector<std::shared_ptr<viewer::GalacticLaunch>>& launches, bool host_saves = false) {
    const auto initial_tick = launches.front()->owner->initial_tick;
    using Live = eawr::platform::LiveGameSession;
    std::vector<std::unique_ptr<Live>> games;
    // #6012: a save made by the host of a lobby-started game, on the start path itself (no hand-set save campaign).
    const auto save_directory = std::filesystem::temp_directory_path() / "eawr-lobby-launch-save-6012";
    if (host_saves) {
        std::filesystem::create_directories(save_directory);
        std::filesystem::remove(save_directory / "lobby save.eawrsave");
    }
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
        expect(launch->owner->session && launch->owner->session->completed_tick() == initial_tick,
            "launch keeps the agreed initial owner");
        Live::Options live; live.pacing = Live::Pacing::driven; live.workers = peer % 2 == 0 ? 1 : 4;
        if (host_saves) live.saves.directory = save_directory.string();
        games.push_back(require(launch->start(std::move(live))));
    }
    constexpr std::uint64_t ticks = 12;
    for (auto& game : games) game->advance_to(initial_tick + ticks);
    for (auto& game : games) expect(game->wait_for(initial_tick + ticks, std::chrono::seconds(20)), "committed GC lobby advances agreed ticks");
    for (std::size_t peer = 0; peer < launches.size(); ++peer) {
        const auto& launch = launches[peer];
        const auto recovery = launch->match->view();
        expect(recovery.recovery_anchor && recovery.recovery_anchor->completed_tick >= initial_tick + ticks
            && recovery.recovery_refusal.empty(),
            "real committed launches retain recovery through their current completed frontier");
        const auto remote = static_cast<net::PeerIndex>((peer + 1) % launches.size());
        auto retained = launch->match->recovery_package(remote,
            {*recovery.recovery_anchor, launch->roster.seats[remote].seat});
        expect(retained.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "the live loading owner answers its retained-checkpoint query");
        const auto package = require(retained.get());
        expect(package.checkpoint.completed_tick == initial_tick
            && package.frontier.completed_tick >= initial_tick + ticks,
            "new and resumed launches retain the initial checkpoint separately from the advancing frontier");
    }
    if (host_saves) {
        // After the recovery checks above: a save refreshes the retained checkpoint, which those checks pin to the initial one.
        eawr::platform::LiveSaveRequest request; request.seat = sim::SeatId{launches.front()->seat}; request.name = "lobby save";
        const auto save_token = games.front()->request_save(request);
        expect(save_token != 0, "the host requests a save of the lobby-started game");
        const auto save_horizon = initial_tick + ticks + 12;
        for (auto& game : games) game->advance_to(save_horizon);
        for (auto& game : games) expect(game->wait_for(save_horizon, std::chrono::seconds(20)), "lobby-started games continue past the save");
        expect(games.front()->wait_save(save_token, std::chrono::seconds(30)), "the lobby-started game's save is published");
        const auto slot = require(eawr::platform::read_save_slot(save_directory.string(), "lobby save"));
        expect(slot.campaign && slot.campaign->multiplayer && slot.campaign->campaign == "Multiplayer" && !slot.campaign->faction.empty()
            && slot.campaign->seat == launches.front()->seat,
            "a save made on the lobby start path records its campaign (campaign_identity), as the lobby's Load Game needs");
        const auto listed = require(eawr::platform::list_saved_games(save_directory));
        expect(std::any_of(listed.begin(), listed.end(), [](const auto& row) {
                   return row.name == "lobby save" && row.campaign && row.campaign->multiplayer && !row.diagnostic; }),
            "the saved game is listed as a loadable multiplayer campaign");
        eawr::testing::remove_temp_directory(save_directory);
    }
    for (auto& game : games) game->stop();
    const auto hashes = games.front()->tick_hashes();
    expect(hashes.size() >= ticks, "all agreed tick hashes were recorded");
    for (const auto& game : games) expect(game->tick_hashes() == hashes, "lobby-launched peers have equal state hashes on every tick");
}

// The mod description accepts some invisible direction marks that the room codec refuses. The host must see that before it
// opens a room, so it can send "not known" instead of a list that would refuse its first guest.
void mod_text_the_room_cannot_carry() {
    eawr::mods::ModDescription plain;
    plain.complete = true;
    plain.entries.push_back({eawr::mods::DescriptionKeyKind::manual, "", "Plain Mod", std::nullopt, std::nullopt});
    const auto fine = eawr::mods::encode_description(plain);
    expect(fine && net::valid_lobby_mod_description(*fine), "an ordinary mod list passes the room codec check");
    auto marked = plain;
    marked.entries.front().name = "Mod" "\xE2\x80\x8E";
    const auto bytes = eawr::mods::encode_description(marked);
    expect(bytes.has_value(), "the description model accepts a left-to-right mark in a name");
    expect(bytes && !net::valid_lobby_mod_description(*bytes), "the room codec check refuses it, so the host falls back to an empty list");
    expect(net::valid_lobby_mod_description({}), "an empty list is always carried");
    if (fine && !fine->empty()) {
        auto old = *fine;
        old[0] = 1;
        expect(!net::valid_lobby_mod_description(old), "the previous mod-description format is refused");
        const std::vector<std::uint8_t> truncated(fine->begin(), fine->end() - 1);
        expect(!net::valid_lobby_mod_description(truncated), "a truncated list is refused");
    }
}

// slow_load: every peer's session build takes this long, as a Remake campaign build does for minutes. With it the
// link and setup timeouts are cut to a fraction of the build, so the room only starts if (1) the room is serviced
// through each build (#6045: the derivation runs on a worker thread and the offer hook answers pending, so this
// thread keeps pumping both peers; #6042's while_pumping still covers the identify and build hooks) and (2) the host
// does not time the guest's preview wait while the guest derives (SetupOptions::untimed_preview_wait). Removing
// either one fails the room. With `slow_load` the host's Start must also return at once and show a pending Start
// with its progress text, and a worker-side derivation never touches the peers (it only sleeps).
void viewer_staging_room(std::uint64_t final_tick_count = 0, bool resume = false,
    std::chrono::milliseconds slow_load = std::chrono::milliseconds::zero(), bool decline = false, bool host_leaves = false,
    bool uncheckable = false) {
    Local host_content, guest_content;
    std::atomic<unsigned> slow_builds{};
    const auto slow_build = [slow_load, &slow_builds] {
        ++slow_builds;
        std::this_thread::sleep_for(slow_load);
    };
    if (slow_load.count() > 0) {
        for (auto* local : {&host_content, &guest_content})
            local->content.prepare_session = [slow_build](const cp::CampaignSetupPlan& plan, std::string_view)
                -> eawr::core::Result<cp::MpPreparedSession> {
                slow_build();
                return eawr::core::Result<cp::MpPreparedSession>::success({
                    std::make_shared<cp::GameSession>(require(cp::GameSession::create(plan.galaxy()))), {}, {},
                    sim::identity_domain("synthetic-runtime", "no external services").digest});
            };
    }
    for (auto* local : {&host_content, &guest_content}) {
        auto solo = local->data.campaigns.front();
        solo.name = "SoloEmpire"; solo.is_multiplayer = false; solo.starting_active_player = "EMPIRE";
        local->data.campaigns.push_back(std::move(solo));
    }
    auto host_backend = std::make_shared<viewer::GalacticLobbyBackend>(host_content.content);
    auto guest_backend = std::make_shared<viewer::GalacticLobbyBackend>(guest_content.content);
    // An uncheckable list is what the host sends when its mod text cannot travel: no bytes, and the room still opens.
    if (uncheckable) host_backend->set_mounted_mods({});
    else host_backend->set_mounted_mods({3, 1, 0, 0, 0, 0});
    expect(host_backend->final_tick_count() == sim::galactic::galactic_max_ticks
        && guest_backend->final_tick_count() == sim::galactic::galactic_max_ticks, "ordinary GC keeps the default session cap");
    if (final_tick_count) {
        host_backend->acceptance_final_tick(final_tick_count);
        guest_backend->acceptance_final_tick(final_tick_count);
    }
    const viewer::LanLobbyBuilder builder = [](const auto&) {
        eawr::core::Diagnostic failure; failure.code = "EAWR-TEST-0002"; failure.message = "a campaign room never builds a skirmish setup";
        return eawr::core::Result<eawr::sim::tactical::TacticalSetup>::failure(std::move(failure));
    };
    auto host = viewer::make_lan_match(), guest = viewer::make_lan_match();
    host->configure_seats({1, 2}); guest->configure_seats({1, 2});
    host->configure_lobby("Host", 0, {}, builder); guest->configure_lobby("Guest", 1, {}, builder);
    host->configure_galactic(host_backend); guest->configure_galactic(guest_backend);
    if (slow_load.count() > 0) {
        const std::chrono::milliseconds cut = slow_load * 4 / 9; // each timeout is under half of a build
        host->set_timeouts_for_test(cut, cut); guest->set_timeouts_for_test(cut, cut);
    }

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
                            ? static_cast<int>(*start->room->setup_phase()) : -1)
                        + " revision=" + std::to_string(state ? state->snapshot.revision : 0)
                        + " ready=" + (state && !state->snapshot.seats.empty() && state->snapshot.seats[0].ready ? "1" : "0")
                        + (state && state->snapshot.seats.size() > 1 && state->snapshot.seats[1].ready ? "1" : "0")
                        + " notice=" + (state && state->notice ? state->notice->message : "none")
                        + " phase=" + std::to_string(static_cast<int>(peer->view().phase))
                        + " reason=" + std::to_string(static_cast<int>(peer->view().reason))
                        + " is_host=" + std::to_string(peer->view().host)
                        + " connected=" + std::to_string(peer->view().connected)
                        + " port=" + std::to_string(peer->view().port)
                        + " code=" + peer->view().diagnostic_code + " " + peer->view().message;
                };
                expect(false, "galactic staging deadline at " + milestone + " resume=" + std::to_string(resume)
                    + " final=" + std::to_string(final_tick_count) + "; host " + describe(host) + "; guest " + describe(guest));
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
    wait([&] { return guest->view().mod_review_pending; });
    expect(guest->view().host_mod_description == (uncheckable ? Bytes{} : Bytes({3, 1, 0, 0, 0, 0})),
        "guest review receives the host's description before staging");
    for (unsigned i = 0; i < 10; ++i) { host->pump_setup(none); guest->pump_setup(none); }
    expect(guest->view().mod_review_pending && !guest->lobby_state()
        && !guest_backend->checkpoint_progress().restored,
        "guest does not process lobby or checkpoint traffic before mod review");
    const auto before_review = host->lobby_state()->snapshot;
    std::vector<net::SetupSeat> pending_bindings;
    for (std::size_t i = 0; i < before_review.seats.size(); ++i)
        pending_bindings.push_back({static_cast<net::PeerIndex>(i), before_review.seats[i].seat, before_review.seats[i].player});
    expect(!host->lobby_galactic_checkpoint(options, pending_bindings)
        && host->lobby_state()->snapshot == before_review,
        "host cannot start checkpoint selection while a guest is reviewing mods");
    if (host_leaves) {
        milestone = "host leaves during review";
        host->cancel();
        wait([&] { return guest->view().phase == viewer::LanPhase::ended; });
        expect(!guest->view().mod_review_pending, "host disconnect ends a pending mod review");
        guest->cancel();
        return;
    }
    if (decline) {
        milestone = "guest declines review";
        guest->lobby_leave(); guest->cancel();
        wait([&] { const auto state = host->lobby_state(); return state && state->snapshot.seats.size() == 1; });
        expect(host->lobby_state()->snapshot.phase == net::LobbyPhase::staging, "declining leaves the host in staging");
        host->cancel();
        return;
    }
    guest->accept_mod_review();
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
    {
        const auto& campaigns = host_backend->campaigns();
        const auto* solo = cp::find_mp_lobby_campaign(campaigns, "SoloEmpire");
        expect(solo != nullptr, "the co-op campaign is available in the room");
        if (solo) {
            const auto coop = require(net::default_galactic_lobby_options(*solo, difficulty, match_seed));
            expect(host->lobby_galactic_game(coop), "host can switch both seats to a different co-op faction");
            wait([&] { return synchronized() && host->lobby_state()->snapshot.game.map == "SoloEmpire"; });
            const auto seats = host->lobby_state()->snapshot.seats;
            expect(seats[0].player == 2 && seats[1].player == 2, "both seats share the authored human faction");
            expect(host->lobby_galactic_game(options), "host can return to multiplayer campaigns");
            wait([&] { return synchronized() && host->lobby_state()->snapshot.game.map == "Multiplayer"; });
            expect(host->lobby_galactic_checkpoint(options, {{0, {11}, 1}, {1, {12}, 2}}), "restore separate rows for later checks");
            wait([&] { return synchronized() && host->lobby_state()->snapshot.seats[0].player == 1; });
        }
    }
    // The checkpoint UI changes rows and owner assignments in one existing lobby revision.
    {
        // Keep the guest's request budget for the faction-move checks below. Its admission/name,
        // two moves and final Ready already exercise the four-request staging window.
        auto colour_choice = host->lobby_state()->snapshot.seats[0].selection;
        colour_choice.colour = 5;
        expect(host->lobby_select(colour_choice), "host selects its profile colour before saved assignment");
        wait(synchronized);
        expect(host->lobby_ready(true), "host accepts before saved mapping changes");
        wait([&] { return synchronized() && host->lobby_state()->snapshot.seats[0].ready; });
        const std::vector<net::SetupSeat> swapped{{0, {12}, 2}, {1, {11}, 1}};
        expect(host->lobby_galactic_checkpoint(options, swapped), "host atomically maps saved factions");
        wait([&] { return synchronized() && host->lobby_state()->snapshot.seats[0].player == 2; });
        expect(host->lobby_state()->snapshot.seats[1].player == 1, "both owners change in the same snapshot");
        expect(host->lobby_state()->snapshot.seats[0].selection.colour == 5,
            "saved faction mapping preserves the host's existing colour choice");
        const auto mapped = host->lobby_state()->snapshot;
        expect(std::none_of(mapped.seats.begin(), mapped.seats.end(), [](const auto& seat) { return seat.ready; }),
            "saved mapping changes clear every human acceptance");
        const auto before = host->lobby_state()->snapshot;
        expect(!guest->lobby_galactic_checkpoint(options, swapped), "joiner cannot assign saved factions");
        auto invalid = swapped; invalid[1].player = 999;
        expect(!host->lobby_galactic_checkpoint(options, invalid) && host->lobby_state()->snapshot == before,
            "invalid saved faction refuses without partially changing the room");
        expect(host->lobby_galactic_checkpoint(options, {{0, {11}, 1}, {1, {12}, 2}}), "host restores the room factions");
        wait([&] { return synchronized() && host->lobby_state()->snapshot.seats[0].player == 1; });
    }
    // A difficulty-only edit must also become a live room revision on the guest.
    auto difficulty_only = options;
    difficulty_only.difficulty = "Hard_Default";
    expect(host->lobby_galactic_game(difficulty_only), "the host edits only difficulty");
    milestone = "difficulty edit";
    wait([&] { return synchronized() && require(net::decode_galactic_lobby_game(guest->lobby_state()->snapshot.game)) == difficulty_only; });
    // A host edit of the options is one shared revision.
    auto changed = options;
    changed.custom = true; changed.credits = 6500; changed.starting_tech = 2; changed.maximum_tech = 4;
    changed.allow_heroes = false; changed.difficulty = "Hard_Default"; changed.auto_resolve = 3; changed.vote_timeout = 45;
    expect(host->lobby_galactic_game(changed), "the host edits the campaign options");
    milestone = "option edit";
    wait([&] { return synchronized() && require(net::decode_galactic_lobby_game(guest->lobby_state()->snapshot.game)) == changed; });
    expect(!guest->lobby_galactic_game(options) && !guest->set_galactic_game(options), "a guest cannot edit the options or the mode");
    {   // Not a multiplayer campaign: refused by the host's validator, the room unchanged.
        const auto before = host->lobby_state()->snapshot;
        auto solo = changed; solo.campaign = "Solo";
        expect(!host->lobby_galactic_game(solo) && host->lobby_state()->snapshot == before, "a campaign outside the multiplayer list is refused");
    }
    {   // Invalid custom settings must not replace the published room.
        const auto before = host->lobby_state()->snapshot;
        auto invalid = changed; invalid.maximum_tech = invalid.starting_tech - 1;
        expect(!host->lobby_galactic_game(invalid) && host->lobby_state()->snapshot == before,
            "inverted custom tech range is refused without publishing");
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

    if (resume) {
        auto roster = require(net::galactic_roster_draft(host->lobby_state()->snapshot, host_content.catalog()));
        for (auto& seat : roster.seats) seat.seat_id.value += 10;
        for (auto& team : roster.teams) team.lead.value += 10;
        cp::MpSessionTerms terms; terms.seed = changed.seed;
        terms.command_quota = static_cast<std::uint32_t>(net::max_batch_commands); terms.byte_quota = static_cast<std::uint32_t>(net::max_batch_bytes);
        auto prepared = require(cp::seal_mp_agreement(host_content.content, roster, terms));
        const sim::InlineExecutor executor;
        for (unsigned tick = 0; tick < 7; ++tick) require(prepared.session->step(executor));
        const auto bytes = std::make_shared<const Bytes>(require(eawr::checkpoint::save_game_checkpoint(*prepared.session,
            eawr::checkpoint::game_identities(prepared.session->galaxy().setup()))));
        require(host_backend->select_checkpoint(host->lobby_state()->snapshot, roster, {{0, {11}, 1}, {1, {12}, 2}}, bytes, 1));
        milestone = "checkpoint transferred and privately restored";
        wait([&] { return host_backend->checkpoint_progress().agreed && guest_backend->checkpoint_progress().restored; });
        expect(host_backend->checkpoint_bytes() && guest_backend->checkpoint_bytes()
            && *guest_backend->checkpoint_bytes() == *bytes,
            "the restored guest retains its verified transfer buffer for recovery at N");
        expect(host_backend->initial_tick() == 7 && guest_backend->initial_tick() == 7, "TCP staging resumes at completed N");
        const auto saved_view = guest_backend->checkpoint_view();
        expect(saved_view && saved_view->completed_tick == 7 && saved_view->bindings[1].seat.value == 12
            && saved_view->peers[1] == guest->view().seat, "joiner reads the checkpoint tick and authenticated saved-seat mapping");
    }

    if (resume) {
        // #6128: the real handshake: only the host ever sees `agreed`, and the lobby's gate must let the guest Accept on its own restore
        // (it blocked the guest for ever, so no saved co-op game could start). From here the guest is present, restored and accepts.
        const auto guest_view = guest_backend->checkpoint_progress(), host_view = host_backend->checkpoint_progress();
        expect(guest_view.restored && !guest_view.agreed && host_view.agreed,
            "#6128 after the restore handshake only the host has agreed; the guest has restored");
        expect(!eawr::viewer::checkpoint_start_blocked(false, false, false, true, guest_view),
            "#6128 the guest's Accept is not blocked after its own restore");
        expect(!eawr::viewer::checkpoint_start_blocked(true, false, false, true, host_view), "#6128 the host's Start is not blocked once every guest restored");
    }
    expect(host->lobby_ready(true) && guest->lobby_ready(true), "both players ready the campaign setup");
    milestone = "ready";
    wait([&] { return host->lobby_state()->snapshot.seats[0].ready && host->lobby_state()->snapshot.seats[1].ready; });
    expect(host_backend->can_start() && !host->galactic_start(), "production adapter is installed, but no launch precedes Start");
    milestone = "Start enabled";
    wait([&] { return host->lobby_state()->start_enabled; });
    const auto start_pressed = std::chrono::steady_clock::now();
    host->start(none);
    const auto start_returned = std::chrono::steady_clock::now() - start_pressed;
    const bool slow = slow_load.count() > 0 && !resume;
    if (slow) {
        // #6045: Start returns at once; the host shows a pending Start, locked, with a progress line the player can read.
        const auto pending = host->lobby_state();
        expect(start_returned < slow_load / 2, "#6045 the host's Start returns at once: the derivation is not on the calling thread");
        expect(pending && pending->start_preparing && !pending->start_enabled && !pending->ready_enabled
            && pending->prepare_status.find("Preparing the campaign for all players") != std::string::npos && !host->galactic_start(),
            "the host shows pending preparation with a progress line before setup is published");
        expect(pending && pending->snapshot.phase == net::LobbyPhase::starting, "the manifest freezes inputs before the host prepares");
        wait([&] { return guest->lobby_state()->snapshot.phase == net::LobbyPhase::starting
            && !guest->lobby_state()->prepare_status.empty(); });
        const auto old_attempt = pending->snapshot.start_attempt;
        // A second press is refused. Cancel invalidates all candidates and a retry requires explicit readiness.
        host->start(none);
        host->cancel_galactic_start();
        expect(!host->lobby_state()->start_preparing, "#6045 Cancel stops waiting");
        wait([&] { return host->lobby_state()->snapshot.phase == net::LobbyPhase::staging
            && guest->lobby_state()->snapshot.phase == net::LobbyPhase::staging; });
        // The earlier faction moves and Ready used the guest's four-request window. This new attempt needs a new
        // explicit Ready; keep pumping until that window expires rather than treating throttled transport as ready.
        milestone = "retry readiness window";
        const auto retry_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(net::LobbyLimits{}.chat_window_ms);
        wait([&] { return std::chrono::steady_clock::now() >= retry_after; });
        expect(host->lobby_ready(true) && guest->lobby_ready(true), "retry explicitly readies both peers again");
        milestone = "retry Start enabled";
        wait([&] { return host->lobby_state()->start_enabled; });
        host->start(none);
        expect(host->lobby_state()->start_preparing && host->lobby_state()->snapshot.start_attempt != old_attempt,
            "retry waits on a fresh attempt and cannot reuse the abandoned candidate");
    }
    milestone = "Start frozen";
    bool guest_status_seen = false, host_waited_visibly = false;
    wait([&] {
        if (const auto guest_state = guest->lobby_state(); guest_state && guest_state->prepare_status.find("Preparing your copy") != std::string::npos)
            guest_status_seen = true;
        if (const auto host_state = host->lobby_state(); host_state && host_state->start_preparing) host_waited_visibly = true;
        return host->galactic_start() && guest->galactic_start() && guest->lobby_state()->snapshot.phase == net::LobbyPhase::starting;
    });
    if (slow) {
        expect(host_waited_visibly && guest_status_seen, "#6045 both seats showed their own progress line while their derivation ran");
        expect(slow_builds.load() >= 3 && slow_builds.load() <= 4, "retry derives both peers anew after the abandoned attempt");
    }
    const auto host_start = *host->galactic_start(), guest_start = *guest->galactic_start();
    expect(host_start.options == changed && host_start.options == guest_start.options && host_start.room && guest_start.room,
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
    verify_launch({host_launch, guest_launch}, true);
    host_launch.reset(); guest_launch.reset();
    host->cancel(); guest->cancel();
}

void viewer_many_seat_launch(std::size_t count, bool lose_during_load = false) {
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
    // #6503: every wait ends on its own event, never on a wall-clock budget. Eight seats deriving their campaigns on load
    // workers took 40-96 s on a busy host, and a Ready sent from a stale room revision is not retried by anyone. A refused or
    // ended peer still fails at once; a true hang is ended by this test's ctest TIMEOUT, and a phase that has made no progress
    // for two minutes prints the per-seat state once so the log says what it waited on.
    const auto wait = [&](const char* phase, const auto& predicate) {
        const auto entered = std::chrono::steady_clock::now();
        bool described = false;
        while (true) {
            for (const auto& peer : peers) {
                peer->pump_setup(none);
                if (peer->view().mod_review_pending) peer->accept_mod_review();
            }
            if (predicate()) return;
            for (const auto& peer : peers) expect(peer->view().phase != viewer::LanPhase::ended,
                "nine-seat campaign connection refused: " + peer->view().message);
            if (!described && std::chrono::steady_clock::now() - entered > std::chrono::minutes(2)) {
                described = true;
                std::cerr << "nine-seat GC launch still waiting in '" << phase << "' after two minutes\n";
                for (std::size_t i = 0; i < peers.size(); ++i) {
                    const auto state = peers[i]->lobby_state();
                    std::cerr << "  seat " << i << " phase=" << static_cast<int>(peers[i]->view().phase);
                    if (state) {
                        std::cerr << " revision=" << state->snapshot.revision << " seats=" << state->snapshot.seats.size()
                                  << " local_ready=" << state->local_ready << " identity_pending=" << state->identity_pending
                                  << " start_enabled=" << state->start_enabled << " preparing=" << state->start_preparing
                                  << " status='" << state->prepare_status << "' start_reason='" << state->start_reason << "'";
                    }
                    std::cerr << "\n";
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    wait("roster", [&] {
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
    // Ready carries the room revision its seat last saw, and every seat's name edit advances that revision. Send Ready only
    // once each seat holds the host's final roster; otherwise a slow seat's Ready is answered "room revision changed" and the
    // host never reaches Start (#6503). Nothing after this point changes the revision until Start.
    wait("settled roster", [&] {
        std::set<std::string> expected;
        for (std::size_t i = 1; i <= count; ++i) expected.insert("Player " + std::to_string(i));
        const auto host_revision = peers[0]->lobby_state()->snapshot.revision;
        for (const auto& peer : peers) {
            const auto state = peer->lobby_state();
            if (!state || state->snapshot.revision != host_revision) return false;
            std::set<std::string> names;
            for (const auto& seat : state->snapshot.seats) names.insert(seat.selection.name);
            if (names != expected) return false;
        }
        return true;
    });
    for (const auto& peer : peers) expect(peer->lobby_ready(true), "multi-seat ready");
    wait("start enabled", [&] { return peers[0]->lobby_state()->start_enabled; });
    peers[0]->start(none);
    std::vector<std::shared_ptr<viewer::GalacticLaunch>> launches(count);
    wait("launches", [&] {
        bool all = true;
        for (std::size_t i = 0; i < count; ++i) {
            if (!launches[i]) launches[i] = peers[i]->take_galactic_launch();
            all = all && static_cast<bool>(launches[i]);
        }
        return all;
    });
    if (lose_during_load) {
        // Every launch owns its stream on a load worker here. Lose one seat
        // before either survivor starts its live driver, then let the galactic
        // departure protocol decide the roster rather than aborting the load.
        peers.back()->cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        expect(launches[0]->finish_load_transport && launches[1]->finish_load_transport,
            "surviving launches retain their load handoffs");
        using Live = eawr::platform::LiveGameSession;
        std::vector<std::unique_ptr<Live>> survivors;
        for (std::size_t i = 0; i < 2; ++i) {
            Live::Options live; live.pacing = Live::Pacing::driven; live.workers = 1;
            survivors.push_back(require(launches[i]->start(std::move(live))));
        }
        for (auto& game : survivors) game->advance_to(12);
        for (auto& game : survivors)
            expect(game->wait_for(12, std::chrono::seconds(20)), "galactic survivors advance after a seat is lost during loading");
        for (auto& game : survivors) game->stop();
        expect(survivors[0]->tick_hashes() == survivors[1]->tick_hashes(),
            "galactic survivors agree after the loading departure");
    } else {
        verify_launch(launches);
    }
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
    const auto prepared_domains = require(cp::prepared_mp_domains(*first));
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
        expect(require(cp::prepared_mp_domains(*agreement)) == prepared_domains,
            "stock peers agree complete owner/provider preparation before 1/8-worker continuation");
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
    const auto prepared_domains = require(cp::prepared_mp_domains(*first));
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
        expect(require(cp::prepared_mp_domains(*agreement)) == prepared_domains,
            "authored mod overlay agrees complete owner/provider preparation across worker/view continuations");
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

// #5572: the room has ONE AI difficulty; every AI seat caption derives from it (never a fixed "Medium").
void ai_seat_caption_follows_room_difficulty() {
    using eawr::net::GalacticAiCaption;
    using eawr::net::galactic_ai_caption;
    expect(galactic_ai_caption("Easy_Default") == GalacticAiCaption::easy, "Easy_Default names the Easy caption");
    expect(galactic_ai_caption("Normal_Default") == GalacticAiCaption::normal, "Normal_Default names the Normal caption");
    expect(galactic_ai_caption("HARD_default") == GalacticAiCaption::hard, "Hard names the Hard caption, any case");
    expect(!galactic_ai_caption("Brutal_Mod") && !galactic_ai_caption(""), "a mod's own difficulty keeps its authored name");
    expect(!galactic_ai_caption("Hardened_Mod") && !galactic_ai_caption("Normal_Brutal") && !galactic_ai_caption("Easyish"),
        "a mod level that merely starts with a stock name keeps its authored name");
    expect(galactic_ai_caption("easy") == GalacticAiCaption::easy && galactic_ai_caption("Medium_Default") == GalacticAiCaption::normal,
        "a bare stock name and Medium_Default are stock levels");
    // Roster level: every AI seat of a room built at a difficulty takes that difficulty's caption, none a fixed Medium.
    const Local local;
    for (const auto& level : {std::pair{"Easy_Default", GalacticAiCaption::easy}, std::pair{"Normal_Default", GalacticAiCaption::normal},
             std::pair{"Hard_Default", GalacticAiCaption::hard}}) {
        const auto options = require(eawr::net::default_galactic_lobby_options(local.catalog().front(), level.first, match_seed));
        std::size_t ai_rows{};
        for (const auto& row : options.rows) {
            if (row.human) continue;
            ++ai_rows;
            expect(galactic_ai_caption(options.difficulty) == level.second, "every AI row of the room takes the room difficulty's caption");
        }
        expect(ai_rows > 0, "the room has an AI row to caption");
    }
}

void faction_captions_and_selection() {
    const auto root = std::filesystem::temp_directory_path() / ("eawr-gc-faction-labels-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{root};
    std::filesystem::create_directories(root / "base/xml");
    std::filesystem::create_directories(root / "mod/xml");
    std::ofstream(root / "base/xml/factionfiles.xml") << "<Faction_Files><File>factions.xml</File></Faction_Files>";
    std::ofstream(root / "base/xml/factions.xml") << "<Factions><Faction Name=\"REBEL\"><Text_ID>BASE</Text_ID></Faction>"
        "<Faction Name=\"EMPIRE\"><Text_ID>BASE</Text_ID></Faction></Factions>";
    std::ofstream(root / "mod/xml/factions.xml") << "<Factions><Faction Name=\"REBEL\"><Text_ID>MOD_NAME</Text_ID></Faction>"
        "<Faction Name=\"EMPIRE\"><Text_ID>MOD_NAME</Text_ID></Faction></Factions>";
    const std::array mounts{eawr::vfs::MountSpec{"mod", root / "mod", "data", {}},
        eawr::vfs::MountSpec{"base", root / "base", "data", {}}};
    const auto files = require(eawr::vfs::Vfs::mount(mounts));
    const auto catalog = require(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    eawr::data::ui::TextDatabase database;
    database.entries.push_back({eawr::data::ui::text_key_crc("MOD_NAME"), "MOD_NAME", u"Republic"});
    database.crc_order.push_back(0);
    eawr::data::ui::TextLookup text(database);
    const Local local;
    const auto host = require(net::default_galactic_lobby_options(local.catalog().front(), difficulty, match_seed));
    const auto joiner = require(net::decode_galactic_lobby_game(require(net::encode_galactic_lobby_game(host))));
    const auto choices = eawr::viewer::galactic::faction_choices(joiner, catalog.catalog, &text);
    expect(choices.size() == 2 && choices[0].caption == "Republic" && choices[1].caption == "Republic",
        "both lobby rows resolve the synthetic mod chain, even with identical captions");
    const auto without_text = eawr::viewer::galactic::faction_choices(joiner, catalog.catalog, nullptr);
    expect(without_text.size() == choices.size(), "missing text lookup retains every lobby faction choice");
    for (const auto& choice : without_text)
        expect(choice.caption == joiner.rows[choice.row].faction,
            "missing text lookup shows the faction identity rather than its Text_ID key");
    std::vector<eawr::presentation::ui::FactionFacts> facts;
    for (const auto& row : host.rows) {
        eawr::presentation::ui::FactionFacts faction{};
        faction.name = row.faction;
        faction.title = eawr::presentation::ui::faction_display_name(catalog.catalog, &text, row.faction);
        faction.playable = row.human;
        facts.push_back(std::move(faction));
    }
    const eawr::presentation::ui::CampaignSetupModel setup({}, std::move(facts));
    expect(setup.factions().size() == choices.size(), "campaign and lobby offer the same synthetic factions");
    for (const auto& choice : choices) {
        const auto& identity = joiner.rows[choice.row].faction;
        const auto campaign = std::find_if(setup.factions().begin(), setup.factions().end(),
            [&](const auto& faction) { return faction.name == identity; });
        expect(campaign != setup.factions().end() && campaign->title == choice.caption,
            "campaign setup and decoded joiner lobby show the same mod caption for each identity");
    }
    expect(eawr::viewer::galactic::faction_choice_player(joiner, choices[0].row) == host.rows[0].player
        && eawr::viewer::galactic::faction_choice_player(joiner, choices[1].row) == host.rows[1].player,
        "identical captions retain distinct host row identities on the joiner");
    expect(!eawr::viewer::galactic::faction_choice_player(joiner, joiner.rows.size()), "invalid picker row is ignored");
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
        checkpoint_buffered_departure();
        checkpoint_failure_stays_visible();
        for (const auto workers : {1U, 2U, 4U, 8U})
            for (const bool sp : {false, true}) production_file_resume(workers, sp);
        for (const auto workers : {1U, 2U, 4U, 8U}) production_file_resume(workers, false, true);
        static_cast<void>(checkpoint_room_resume(1, 2, false, false, 1));
        static_cast<void>(checkpoint_room_resume(1, 2, false, false, 2));
        static_cast<void>(checkpoint_room_resume(1, 3, false, false, 3));
        guest_restore_runs_off_the_ui_thread();
        free_faction_choice();
        std::string resume_reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) for (const auto count : {2U, 3U}) for (const bool relay : {false, true}) {
            const auto hash = checkpoint_room_resume(workers, count, relay);
            if (workers == 1 && count == 2 && !relay) resume_reference = hash;
            if (count == 2) expect(hash == resume_reference, "resumed state is equal across transport profiles and 1/2/4/8 workers");
        }
        for (const auto workers : {1U, 2U, 4U, 8U})
            for (const bool relay : {false, true}) static_cast<void>(checkpoint_room_resume(workers, 2, relay, true));
        for (const unsigned failure : {1U, 2U, 3U})
            static_cast<void>(checkpoint_room_resume(1, failure == 3 ? 3 : 2, false, false, failure, true));
        static_cast<void>(checkpoint_room_resume(1, 2, false, true, 0, true));
        std::string sp_resume_reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const auto hash = checkpoint_room_resume(workers, 2, false, false, 0, true);
            if (workers == 1) sp_resume_reference = hash;
            else expect(hash == sp_resume_reference, "SP co-op resume is equal across worker counts");
        }
        codec_and_mode();
        faction_captions_and_selection();
        skirmish_bytes_pinned();
        content_load_never_blocks_the_ui_thread();
        campaign_list();
        coop_catalog_without_multiplayer_campaign();
        coop_single_player_sets();
        campaign_labels();
        staging_validation();
        difficulty_follows_the_campaigns_ai_types();
        ai_seat_caption_follows_room_difficulty();
        roster_draft();
        two_peer_room();
        saved_mapping_clears_every_acceptance();
        host_toggles_row_open_ai();
        nine_seat_room();
        two_sides_with_extra_ai();
        content_mismatch_returns_to_staging();
        mounted_mod_warning();
        refused_start_is_diagnosed();
        setup_failure_texts();
        start_summary_compares_peers();
        combined_agreement_identity();
        preparation_attempt_lifetime();
        asynchronous_offer();
        asynchronous_offer_failure();
        asynchronous_offer_frees_the_previous_preparation();
        wire_identity_and_preparation_keys();
        reoffer_keeps_agreed_services();
        nonce_bound_preparation();
        preparation_content_manifest();
        late_preparation_content_reads();
        cached_model_content_refresh();
        acceptance_final_tick_agrees();
        viewer_staging_room();
        viewer_staging_room(0, false, {}, true);
        viewer_staging_room(0, false, {}, false, true);
        viewer_staging_room(0, false, {}, false, false, true);
        mod_text_the_room_cannot_carry();
        viewer_staging_room(12000);
        viewer_staging_room(0, true);
        viewer_staging_room(0, false, std::chrono::milliseconds(4500));
        viewer_many_seat_launch(9);
        viewer_many_seat_launch(8);
        viewer_many_seat_launch(3, true);
        installed_observer_agreement();
        viewer::shutdown_lan_drains();
        std::cout << "galactic lobby contracts passed: gc2 codec and versioning, campaign list, staging refusals, two-peer and nine-seat rooms, skirmish bytes pinned\n";
        return 0;
    } catch (const std::exception& error) {
        viewer::shutdown_lan_drains();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
