#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "../../apps/viewer/src/galactic/campaign_capture_metrics.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <limits>
#include <exception>
#include <set>
using eawr::testing::require;
using eawr::testing::take;

bool galactic_support_self_test();

namespace {
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
using Fixed = eawr::sim::math::Fixed;

Fixed f(std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }
gc::GalacticSetup setup() {
    gc::GalacticSetup s;
    s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    s.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}, {{2}, 2, gc::SeatKind::ai, 1, true, 0}, {{3}, 1, gc::SeatKind::script, 2, true, 0}};
    s.planets = {{1, {}, 1}, {2, {f(10), {}, {}}, 2}, {3, {f(9), {}, {}}, 3}, {4, {f(200), {}, {}}, 3}};
    s.routes = {{5, 1, 4, f(1)}};
    s.fleets = {{10, 1, 2, {{11, 100}}, 0, {}}};
    s.debug_commands = true;
    gc::ObserverSetup o; o.human_observers = {1}; o.solo_reference_human = 1; o.reveal_delay_seconds = take(Fixed::from_decimal("0.5")); o.automatic_reveal_range = f(10);
    o.players = {{1, false, false, 0, 3, {}}, {2, true, false, gc::bit(gc::Intelligence::ownership) | gc::bit(gc::Intelligence::fleet_presence), 1, {}}, {3, false, false, 0, 1, {}}};
    for (const auto& p : s.planets) {
        o.planets.push_back({p.id, p.owner, p.owner, 0, 0, 0, 0, false, false, false, false, {}});
        o.planets.back().current_owner = o.planets.back().current_controller = p.owner;
    }
    o.types = {{100, false, false, false}};
    for (const auto& player : s.players) for (const auto& p : s.planets) o.records.push_back({player.player_id, p.id, false, 0, 0, 0, {}});
    s.observer = std::move(o); return s;
}
gc::ObserverCommand effect(gc::ObserverAction action, std::uint64_t frame, std::uint32_t mask = gc::bit(gc::Intelligence::fleet_contents)) {
    gc::ObserverCommand c; c.action = action; c.player = 1; c.planet = 4; c.gameplay_frame = frame;
    c.source = {1, 1, "synthetic intelligence source", mask, 0}; c.duration = f(1); return c;
}
void submit(gc::GalacticSession& session, std::uint64_t sequence, gc::ObserverCommand c, gc::SeatId seat = {3}) {
    require(session.submit({{session.completed_tick(), seat, sequence}, std::move(c)}));
}
const gc::ObservedPlanet& view(const gc::GalacticSession& session, std::size_t index) { return session.snapshot()->observations()[index]; }

void campaign_home_expansion() {
    // WGS-06 installs the home; WGUI-01/04/107 independently qualify seeds.
    // A retail owned home can expand on the first service even while paused.
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const bool owned : {false, true}) {
        auto s = setup(); s.fleets.clear(); s.observer->records.clear();
        s.observer->planets[0].automatic_reveal = true;
        s.planets[0].owner = owned ? 1 : 3;
        auto& home = s.observer->planets[0];
        home.allegiance = home.target_owner = home.current_owner = home.current_controller = s.planets[0].owner;
        require(gc::initialize_campaign_observer(s, 1));
        auto game = take(gc::GalacticSession::create(s));
        expect(view(game, 0).discovered && !view(game, 2).discovered && !view(game, 3).discovered,
            "WGS-06 explicit startup home is distinct from the first ordinary discovery pass");
        require(game.submit({{0, {1}, 1}, gc::PauseGalaxy{}}));
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        (void)take(game.step(executor));
        expect(!view(game, 1).discovered && view(game, 2).discovered == owned && view(game, 3).discovered == owned,
            "WGUI-04/107 paused owned home expands nearby and trade endpoints; discovery alone does not qualify a neutral preview home");
        (void)take(game.step(executor));
        expect(!view(game, 1).discovered,
            "WGUI-107 newly revealed foreign endpoint does not recursively expand to its nearby exact-boundary neighbour");
    }
}

void solo_observer_publication() {
    auto s = setup();
    auto game = take(gc::GalacticSession::create(s));
    const auto check = [&] {
        const auto canonical = game.snapshot();
        const auto publication = game.view_snapshot({1});
        expect(publication && publication->view_player() == 1
            && publication->observations().size() == s.planets.size(),
            "solo reference publication contains only its explicit human's observations");
        std::vector<gc::ObservedPlanet> expected;
        for (const auto& row : canonical->observations()) if (row.player == 1) expected.push_back(row);
        expect(std::ranges::equal(publication->observations(), expected),
            "solo private rows preserve the canonical reference-human knowledge exactly");
        expect(publication->native_root() == canonical.get()
            && publication->sha256() == canonical->sha256(),
            "solo private publication shares the completed immutable state and derived services");
        expect(game.view_snapshot({1})->observations().data() == publication->observations().data(),
            "unchanged solo observer inputs reuse the private row vector");
        expect(!game.view_snapshot({2}), "solo projection still refuses a nonhuman view");
    };
    check();
    submit(game, 1, effect(gc::ObserverAction::add, 0));
    const eawr::sim::InlineExecutor executor;
    (void)take(game.step(executor));
    check();
    expect(game.view_snapshot({1})->observations()[3].knowledge.fleet_contents,
        "solo reference publication refreshes its filtered rows after an observer change");
}

void multiplayer_observer_contexts() {
    std::vector<std::string> baseline;
    std::vector<std::uint8_t> replay;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto s = setup();
        s.seats[1].kind = gc::SeatKind::human; s.seats[1].human_side_id = 2;
        s.observer->human_observers = {1, 2}; s.observer->solo_reference_human = 0;
        s.observer->players[1].ai = false; s.observer->players[2].ai = true;
        s.observer->records.clear(); s.fleets[0].orbit = 1;
        const std::array<gc::ObserverHome, 2> homes{{{1, 1}, {2, 2}}};
        require(gc::initialize_campaign_observers(s, homes));
        auto owner = take(eawr::campaign::GameSession::create(s));
        auto& game = owner.galaxy();
        const auto canonical = game.snapshot()->sha256();
        const auto host = game.view_snapshot({1});
        const auto guest = game.view_snapshot({2});
        expect(host && guest && host->view_player() == 1 && guest->view_player() == 2,
            "each human queries its own view without a canonical local selector");
        expect(host->observations().size() == s.planets.size() && guest->observations().size() == s.planets.size(),
            "private observer publications contain only the requested player's rows");
        expect(host->observations()[0].discovered && !host->observations()[1].discovered
            && guest->observations()[1].discovered && !guest->observations()[0].discovered,
            "canonical startup reveals each human's distinct home on every peer");
        expect(game.view_snapshot({1})->observations().data() == host->observations().data()
            && game.view_snapshot({2})->observations().data() == guest->observations().data(),
            "seat switching reuses independent player-keyed caches");
        expect(host->sha256() == canonical && guest->sha256() == canonical && game.snapshot()->sha256() == canonical,
            "view context and its caches never enter canonical snapshot bytes");
        expect(!game.view_snapshot({3}) && !game.view_snapshot({999}), "nonhuman and unknown view contexts fail closed");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes{owner.state_sha256()};
        for (int tick = 0; tick < 8; ++tick) {
            (void)game.view_snapshot({tick % 2 ? 1U : 2U});
            hashes.push_back(take(owner.step(executor)).state_hash.get());
        }
        auto ids = eawr::checkpoint::game_identities(game.setup());
        const auto saved = take(eawr::checkpoint::save_game_checkpoint(owner, ids));
        auto exact = take(eawr::checkpoint::load_game_checkpoint(saved, ids, {}, executor));
        expect(exact.session.state_sha256() == owner.state_sha256(), "resync restores all humans' canonical history exactly");
        expect(std::ranges::equal(game.view_snapshot({2})->observations(), exact.session.galaxy().view_snapshot({2})->observations()),
            "resync reconstructs the requesting guest's private view");
        eawr::checkpoint::GameLoadOptions options; options.player_load = true;
        auto loaded = take(eawr::checkpoint::load_game_checkpoint(saved, ids, options, executor));
        const auto& records = loaded.session.galaxy().snapshot()->observer()->records;
        for (const auto& row : records) if (row.player != 3)
            expect(row.apparent_owner != 0, "player-load refresh visits both canonical human contexts");
        const auto bytes = take(gc::write_replay(game.record()));
        auto old_rules = bytes; old_rules[12] = 1;
        expect(!gc::parse_replay(old_rules), "replays with the old observer/rules identity are explicitly rejected");
        if (workers == 1) { baseline = hashes; replay = bytes; }
        else expect(hashes == baseline && bytes == replay, "all-human startup, ticks and replay bytes equal on 1/2/4/8 workers");
    }
    auto invalid = setup(); invalid.observer->human_observers = {2, 1};
    expect(!gc::validate_observer(invalid), "unordered canonical humans are rejected");
    invalid = setup(); invalid.observer->human_observers = {1, 2};
    expect(!gc::validate_observer(invalid), "multihuman policy rejects a solo reference human");
}

void predicates() {
    expect(gc::owner_intelligence == 40959 && gc::bit(gc::Intelligence::ground_level) == 2
        && gc::bit(gc::Intelligence::space_level) == 4, "WGUI-98 numeric categories and owned default");
    expect(gc::faction_default_intelligence(41, 41, 99) == 17 && gc::faction_default_intelligence(99, 41, 99) == 17
        && gc::faction_default_intelligence(12, 41, 99) == 0 && gc::faction_default_intelligence(2, {}, {}) == 0,
        "WGUI-98 good and evil defaults use canonical definition IDs, not faction ordinals");
    gc::IntelligenceContext c; c.observer = 2; c.local_player = 1; c.allegiance = 3; c.minimum_visible_level = 3;
    auto k = gc::planet_knowledge(c);
    expect(!k.ownership && !k.fleet_presence && k.superweapon, "WGUI-12/17: unknown neutral ownership, unconditional superweapon predicate");
    c.allegiance = 1;
    expect(gc::planet_knowledge(c).ownership, "WGUI-12: ownership uses local player even for another observer"); c.allegiance = 3;
    c.corrupted = c.corruption_benefit = true; k = gc::planet_knowledge(c);
    expect(k.ownership && k.production && k.fleet_contents && k.structures && k.minor_stealth, "corruption grants only predicates with its bypass");
    expect(!k.income && !k.income_breakdown && !k.force_sensitive && !k.strongest_ship, "WGUI-13/16: corruption does not grant income, force-sensitive or strongest ship");
    c.corrupted = false; c.space_stealth = true; c.space_owner = 3; c.mask = gc::all_intelligence; k = gc::planet_knowledge(c);
    expect(!k.fleet_presence && !k.fleet_count && !k.fleet_contents && !k.structures && k.ground_contents, "WGUI-10/14/15: separate stealth gates cannot be flattened into one mask");
    c.allied_space_owner = true; k = gc::planet_knowledge(c);
    expect(k.fleet_contents && k.structures, "allied base owner bypasses potential-content gate");
    c = {}; c.local_player = 1; c.allegiance = 3; c.minimum_visible_level = 3; c.space_level = 2;
    expect(!gc::planet_knowledge(c).space_level, "base below faction threshold unknown"); c.space_level = 3;
    expect(gc::planet_knowledge(c).space_level && gc::planet_knowledge(c).ownership, "WGUI-11/12: threshold equality reveals base and nonzero base owner information");
    c.space_level = 0; c.attached_superweapon = true; k = gc::planet_knowledge(c);
    expect(k.fleet_count && k.fleet_presence && !k.fleet_contents, "WGUI-14/17: superweapon reveals count independently of contents");
    gc::ObjectIntelligenceContext object; object.planetary_system = true; object.hero = true; object.stealth_hero = true; object.container = gc::IntelligenceContainer::orbit;
    k.fleet_contents = true;
    expect(!gc::object_details_known(object, k), "stealth hero is not revealed merely by fleet contents"); k.minor_stealth = true;
    expect(gc::object_details_known(object, k), "minor hero detection"); object.named_hero = true;
    expect(!gc::object_details_known(object, k), "named hero requires major detection"); object.allied = true; object.planetary_system = false;
    expect(gc::object_details_known(object, k), "allies accepted before planetary system requirement");
    auto spy = gc::validated_spy({gc::SpyStyle::galactic_automatic, f(60), gc::bit(gc::Intelligence::fleet_contents) | gc::bit(gc::Intelligence::income_breakdown)});
    expect(spy.duration == f(-1) && (spy.mask & gc::bit(gc::Intelligence::fleet_count)) && (spy.mask & gc::bit(gc::Intelligence::income)), "WGUI-19/20: automatic duration and prerequisites");
    expect(gc::validated_spy({gc::SpyStyle::ground_activated, f(-1), 0}).duration == f(30), "ground duration default is sourced");
    expect(take(gc::intelligence_expiry(f(-1), 10)) == std::numeric_limits<std::uint64_t>::max(), "permanent intelligence expiry");
    expect(take(gc::intelligence_expiry(Fixed::from_raw(Fixed::scale / 60 + 1), 10)) == 11, "positive expiry half-frame rounding");
    expect(!gc::intelligence_expiry(f(1), std::numeric_limits<std::uint64_t>::max()), "expiry rejects overflow");
}

void discovery() {
    eawr::sim::InlineExecutor executor;
    auto game = take(gc::GalacticSession::create(setup()));
    expect(!view(game, 1).discovered && !view(game, 3).known_owner, "tick-zero discovery is explicit, not guessed from raw ownership");
    for (int i = 0; i < 16; ++i) { (void)take(game.step(executor)); expect(!view(game, 1).discovered, "first sixteen eligible calls only increment"); }
    (void)take(game.step(executor)); expect(view(game, 1).discovered, "seventeenth call reveals from prior counter greater than 15");
    expect(view(game, 0).known_owner == 1U && view(game, 0).knowledge.income, "local owner intelligence published independently of neutral defaults");
    auto paused = take(gc::GalacticSession::create(setup()));
    (void)take(paused.step(executor)); require(paused.submit({{1, {1}, 1}, gc::PauseGalaxy{}}));
    (void)take(paused.step(executor)); expect(paused.snapshot()->observer()->records[1].reveal_counter == 0, "WGUI-01 pause resets orbit counter");
    auto s = setup(); s.observer->types[0].stealth_hero = true;
    auto hidden = take(gc::GalacticSession::create(s)); for (int i = 0; i < 20; ++i) (void)take(hidden.step(executor));
    expect(!view(hidden, 1).discovered, "stealth-only force does not discover");
    s = setup(); s.observer->planets[0].automatic_reveal = true;
    auto spread = take(gc::GalacticSession::create(s)); (void)take(spread.step(executor));
    expect(!view(spread, 1).discovered && view(spread, 2).discovered && view(spread, 3).discovered, "WGUI-04 exact boundary excluded, nearby foreign candidate and long trade endpoint revealed");
    s = setup(); s.observer->players[0].benefits_from_corruption = true; s.observer->planets[3].corrupted = true;
    auto corruption = take(gc::GalacticSession::create(s)); (void)take(corruption.step(executor));
    expect(view(corruption, 3).discovered && view(corruption, 3).knowledge.production, "WGUI-03 corruption-benefiting player discovers neutral planet");
    s = setup(); s.observer->logical_fps = 60;
    auto rate = take(gc::GalacticSession::create(s));
    for (int i = 0; i < 31; ++i) (void)take(rate.step(executor));
    expect(!view(rate, 1).discovered, "authored half-second discovery threshold follows synchronized logical FPS");
    (void)take(rate.step(executor)); expect(view(rate, 1).discovered, "strict old-counter reveal at 60 FPS requires call32");
}

void arrival_order_and_budget() {
    auto s = setup(); s.fleets[0].orbit = 1;
    s.fleets[0].motion.facing = {f(1), {}, {}}; // WGM-29: prealign to isolate same-tick arrival, not a turn tick.
    s.observer->reveal_delay_seconds = Fixed::from_raw(Fixed::scale / 30 + 1);
    s.rules.maximum_fleet_movement_distance = f(300);
    s.fleet_control.emplace(); s.fleet_control->types = {{100, false, true, false}};
    s.movement.emplace(); auto& m = *s.movement;
    m.mode_modifier = f(1); m.types = {{100, f(1000), f(1), 0, true, false, false, false}};
    for (const auto& p : s.players) m.players.push_back({p.player_id, f(1), {}, false});
    for (const auto& p : s.planets) m.planets.push_back({p.id, {}, false});
    for (const auto& p : s.players) for (const auto& q : s.planets) m.access.push_back({p.player_id, q.id, true, false});
    eawr::sim::InlineExecutor serial; galactic_test::PhaseProbe executor(serial);
    auto game = take(gc::GalacticSession::create(s));
    require(game.submit({{0, {1}, 1}, gc::MoveFleet{10, {1, 2}, {}}}));
    const auto first = take(game.step(executor));
    expect(game.snapshot()->fleets()[0].orbit == 2 && !game.snapshot()->fleets()[0].motion.transit,
        "synthetic high-speed fleet arrives during first object service");
    expect(first.snapshot->observer()->records[1].reveal_counter == 0, "GFSC-13 arrival cannot increment the prior discovery pass");
    const auto discovery_phase = std::find_if(executor.phases.begin(), executor.phases.end(), [](const auto& p) { return p.first == "galactic-discovery"; });
    const auto movement_phase = std::find_if(executor.phases.begin(), executor.phases.end(), [](const auto& p) { return p.first == "galactic-locomotion"; });
    expect(discovery_phase < movement_phase, "GFD-23 discovery phase precedes locomotion");
    (void)take(game.step(executor));
    expect(game.snapshot()->observer()->records[1].reveal_counter == 1 && !view(game, 1).discovered, "GFSC-13 next pass increments 0 to 1");
    (void)take(game.step(executor)); expect(game.snapshot()->observer()->records[1].reveal_counter == 2 && !view(game, 1).discovered, "GFSC-13 equality increments 1 to 2");
    const auto revealed = take(game.step(executor)); expect(view(game, 1).discovered, "GFSC-13 fourth pass reveals from counter2 greater than threshold1");
    expect(revealed.work.discovery_records == s.observer->records.size() && revealed.work.discovery_targets == 0,
        "discovery work visits each observer record once and performs no geometry queries in the tick");
}

void landed_ownership() {
    auto s = setup(); auto& planet = s.observer->planets[3]; planet.space_stealth = true;
    s.observer->records[3].mask = gc::bit(gc::Intelligence::ground_contents);
    planet.landed_transports = {{0, 0, false, false}, {100, 2, true, false}};
    auto empty = take(gc::GalacticSession::create(s));
    expect(!view(empty, 3).knowledge.ownership, "WGUI-103 null and delete-pending transports do not expose ownership");
    planet.landed_transports.push_back({101, 2, false, true});
    auto dead = take(gc::GalacticSession::create(s));
    expect(view(dead, 3).knowledge.ownership, "WGUI-103 nonnull non-delete-pending enemy transport counts even when marked dead");
    planet.landed_transports.clear(); s.observer->records[3].mask = gc::bit(gc::Intelligence::ownership);
    auto hidden = take(gc::GalacticSession::create(s)); expect(!view(hidden, 3).knowledge.ownership, "WGUI-12 starbase stealth alone blocks ownership bit");
    planet.space_stealth = false; planet.ground_stealth = true;
    auto ground = take(gc::GalacticSession::create(s)); expect(!view(ground, 3).knowledge.ownership, "WGUI-12 ground stealth alone blocks ownership bit");
    planet.ground_stealth = false;
    auto normal = take(gc::GalacticSession::create(s)); expect(view(normal, 3).knowledge.ownership, "ownership bit succeeds with neither stealth flag");
}

void effects() {
    eawr::sim::InlineExecutor executor; auto game = take(gc::GalacticSession::create(setup()));
    submit(game, 1, effect(gc::ObserverAction::add, 100)); (void)take(game.step(executor));
    expect(view(game, 3).knowledge.fleet_contents && !view(game, 3).discovered, "spy intelligence does not substitute for discovery");
    gc::ObserverCommand refresh; refresh.action = gc::ObserverAction::refresh; refresh.player = 1; refresh.planet = 4; refresh.gameplay_frame = 130;
    submit(game, 2, refresh); (void)take(game.step(executor)); expect(view(game, 3).knowledge.fleet_contents, "expiry equality retains source");
    refresh.gameplay_frame = 131; submit(game, 3, refresh); (void)take(game.step(executor)); expect(!view(game, 3).knowledge.fleet_contents, "strictly later refresh expires source");
    auto first = effect(gc::ObserverAction::add, 132); first.duration = f(-1); submit(game, 4, first);
    auto second = first; second.source.ability = 2; second.source.mask = gc::bit(gc::Intelligence::income); submit(game, 5, second);
    (void)take(game.step(executor));
    first.action = gc::ObserverAction::remove; submit(game, 6, first); (void)take(game.step(executor));
    expect(!view(game, 3).knowledge.fleet_contents && view(game, 3).knowledge.income, "WGUI-22 removing one identity preserves overlapping independent spy");
    const auto before = game.snapshot()->observer()->records[3]; auto overflow = second; overflow.gameplay_frame = std::numeric_limits<std::uint64_t>::max(); overflow.duration = f(1);
    submit(game, 7, overflow); (void)take(game.step(executor));
    expect(game.snapshot()->observer()->records[3] == before && game.snapshot()->events().front().reason == gc::GalacticRejectReason::observer_input, "overflowing source expiry leaves target source/memory state unchanged");
    submit(game, 8, second, {1}); (void)take(game.step(executor));
    const auto& denied = game.snapshot()->events().front();
    expect(denied.reason == gc::GalacticRejectReason::not_authorized && denied.subject == second.planet && denied.value == second.player,
        "human cannot fabricate observer facts; refusal retains the requested planet and player");
    auto transfer = effect(gc::ObserverAction::transfer, 132); transfer.planet = 1; transfer.previous_player = 1; transfer.player = 2;
    transfer.source.source = 1; transfer.duration = f(-1); transfer.influence_range = f(10);
    submit(game, 9, transfer); (void)take(game.step(executor));
    expect(game.snapshot()->observer()->records[4].sources.size() == 1 && game.snapshot()->observer()->records[6].sources.size() == 1
        && game.snapshot()->observer()->records[5].sources.empty(), "WGUI-24 planet-self transfer uses strict nearby range");
    auto bad = setup(); bad.observer->records.pop_back(); expect(!gc::GalacticSession::create(bad), "incomplete observer table refused");
}

void staggered_observer_sources() {
    // WGUI-21/22: independent ability identities retain overlapping knowledge.
    // Discovery persists separately after the last source leaves (WGUI-03).
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto s = setup(); s.observer->records[3].discovered = true;
        auto game = take(gc::GalacticSession::create(s));
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto first = effect(gc::ObserverAction::add, 0,
            gc::bit(gc::Intelligence::ownership) | gc::bit(gc::Intelligence::ground_contents));
        // WGUI-21 admits existing sources: planet 1 and fleet 10 both belong to player 1.
        first.source.source = 1; first.duration = f(-1);
        auto second = first; second.source.source = 10;
        submit(game, 1, first); (void)take(game.step(executor));
        expect(view(game, 3).known_owner == gc::PlayerId{3} && view(game, 3).knowledge.ground_contents,
            "first independent source grants current owner and contents");
        submit(game, 2, second); (void)take(game.step(executor));
        first.action = gc::ObserverAction::remove;
        submit(game, 3, first); (void)take(game.step(executor));
        expect(view(game, 3).known_owner == gc::PlayerId{3} && view(game, 3).knowledge.ground_contents
            && game.snapshot()->observer()->records[3].sources.size() == 1,
            "withdrawing one overlapping source preserves the second source's knowledge");
        second.action = gc::ObserverAction::remove;
        submit(game, 4, second); (void)take(game.step(executor));
        expect(view(game, 3).discovered && !view(game, 3).known_owner && !view(game, 3).knowledge.ground_contents
            && game.snapshot()->observer()->records[3].sources.empty(),
            "last withdrawal removes current owner/contents knowledge without revoking discovery");
    }
}

void apparent_memory() {
    eawr::sim::InlineExecutor executor;
    auto s = setup(); auto& initial = s.observer->records[3];
    const auto mask = gc::bit(gc::Intelligence::ownership) | gc::bit(gc::Intelligence::political_control);
    initial.mask = mask; initial.sources = {{1, 1, "expiry memory fixture", mask, 30}};
    initial.apparent_owner = 2; initial.apparent_control = f(5);
    s.observer->planets[3].political_control = f(2);
    auto game = take(gc::GalacticSession::create(s));
    gc::ObserverCommand refresh; refresh.action = gc::ObserverAction::refresh; refresh.player = 1; refresh.planet = 4;
    refresh.gameplay_frame = 31; refresh.clear_local = false;
    submit(game, 1, refresh); (void)take(game.step(executor));
    expect(!view(game, 3).known_owner && !view(game, 3).known_political_control
        && view(game, 3).apparent_owner == 3U && view(game, 3).apparent_political_control == f(2),
        "WGUI-100 direct loss caches CURRENT actual values, not previous visible observations");
    refresh.clear_local = true; refresh.gameplay_frame = 32; submit(game, 2, refresh); (void)take(game.step(executor));
    expect(!view(game, 3).apparent_owner && view(game, 3).apparent_political_control == Fixed{}
        && game.snapshot()->observer()->records[3].apparent_owner == -1, "ordinary refresh clears apparent memory before recomputation");
    s = setup(); s.observer->records[7].mask = gc::bit(gc::Intelligence::income);
    s.observer->records[7].sources = {{1, 2, "independent AI source", gc::bit(gc::Intelligence::income), 10}};
    auto generic = take(gc::GalacticSession::create(s)); auto c = effect(gc::ObserverAction::add, 20); c.spy_effect = false;
    submit(generic, 1, c); (void)take(generic.step(executor));
    expect(view(generic, 7).knowledge.income, "WGUI-22 generic modifier refreshes only affected observer");
    c.spy_effect = true; c.gameplay_frame = 21; submit(generic, 2, c); (void)take(generic.step(executor));
    expect(!view(generic, 7).knowledge.income, "WGUI-21 spy effect refreshes every observer at the target");
    s = setup(); s.observer->planets[1].political_control = f(1);
    auto orbit = take(gc::GalacticSession::create(s));
    expect(!view(orbit, 1).knowledge.political_control && view(orbit, 1).apparent_political_control == f(1)
        && view(orbit, 1).apparent_allegiance_owner == 2U, "WGUI-100 non-Empire own orbit returns actual allegiance independently of political permission");
    s.observer->players[0].canonical_empire = true;
    auto empire = take(gc::GalacticSession::create(s));
    expect(view(empire, 1).apparent_political_control == Fixed{} && !view(empire, 1).apparent_allegiance_owner,
        "canonical Empire own orbit returns neutral apparent allegiance at integer control >=1");
    s.observer->planets[1].political_control = {};
    auto low = take(gc::GalacticSession::create(s));
    expect(view(low, 1).apparent_allegiance_owner == 2U, "Empire orbit returns actual allegiance below integer control1");
    s = setup(); s.fleets.clear(); s.observer->planets[1].target_owner = 1;
    auto target = take(gc::GalacticSession::create(s));
    gc::ObserverCommand visible; visible.action = gc::ObserverAction::refresh; visible.player = 1; visible.planet = 2;
    submit(target, 1, visible); (void)take(target.step(executor));
    expect(view(target, 1).knowledge.ownership && view(target, 1).known_owner == 2U,
        "default source uses target-control owner while known-owner getter returns CURRENT actual owner");
}

void source_lifecycle() {
    auto config = setup(); config.observer->records[2].apparent_owner = 2;
    config.observer->records[2].apparent_control = f(5);
    eawr::sim::InlineExecutor executor; auto game = take(gc::GalacticSession::create(config));
    auto c = effect(gc::ObserverAction::influence_insert, 20); c.player = 2; c.enabled = true;
    c.style = gc::SpyStyle::galactic_automatic; c.duration = f(60);
    submit(game, 1, c); (void)take(game.step(executor));
    expect(view(game, 3).knowledge.fleet_contents && game.snapshot()->observer()->records[3].sources[0].final_frame == std::numeric_limits<std::uint64_t>::max()
        && game.snapshot()->observer()->records[7].sources.empty(), "WGUI-101 enabled automatic insertion derives source owner and validates permanent duration");
    require(game.submit({{game.completed_tick(), {1}, 1}, gc::DebugSetPlanetOwner{1, 2}})); (void)take(game.step(executor));
    c.action = gc::ObserverAction::influence_remove; submit(game, 2, c); (void)take(game.step(executor));
    expect(view(game, 3).knowledge.fleet_contents, "influence removal uses source CURRENT owner, not the prior recipient");
    c.action = gc::ObserverAction::influence_insert; c.enabled = false; submit(game, 3, c); (void)take(game.step(executor));
    expect(game.snapshot()->observer()->records[7].sources.empty(), "disabled automatic effect does not insert");
    c = effect(gc::ObserverAction::add, 21); c.source.source = 11; c.source.ability = 9; c.source.mask = gc::bit(gc::Intelligence::income); c.duration = f(1);
    submit(game, 4, c); (void)take(game.step(executor));
    c.action = gc::ObserverAction::terminate; c.despawning = true; c.owner_destroyed = true; submit(game, 5, c); (void)take(game.step(executor));
    expect(view(game, 3).knowledge.income, "despawning consumed probe leaves timed intelligence attached");
    gc::ObserverCommand refresh; refresh.action = gc::ObserverAction::refresh; refresh.player = 1; refresh.planet = 4; refresh.gameplay_frame = 52;
    submit(game, 6, refresh); (void)take(game.step(executor));
    expect(!view(game, 3).knowledge.income, "retained probe effect still expires at its own explicit clock boundary");
    c = effect(gc::ObserverAction::add, 200); c.source.source = 11; c.source.ability = 12; c.source.mask = gc::bit(gc::Intelligence::income); c.duration = f(1); c.logical_fps = 60;
    submit(game, 7, c); (void)take(game.step(executor));
    expect(game.snapshot()->observer()->records[3].sources.back().final_frame == 260, "WGUI-102 expiry reads synchronizer logical FPS");
    refresh.gameplay_frame = 20; submit(game, 8, refresh); (void)take(game.step(executor));
    expect(view(game, 3).knowledge.income && game.snapshot()->observer()->records[3].refresh_frame == 20,
        "linked tactical refresh accepts a lower active mode clock without substituting retained GC time");
    c.action = gc::ObserverAction::terminate; c.despawning = true; c.owner_destroyed = false;
    submit(game, 9, c); (void)take(game.step(executor));
    expect(!view(game, 3).knowledge.income, "ordinary termination cleans up even for a despawning ability; only owner-destroyed branch preserves it");
    expect(game.snapshot()->observer()->records[2].apparent_owner == 2 && game.snapshot()->observer()->records[2].apparent_control == f(5),
        "termination only refreshes targets where matching effects were actually removed");
}

void projection_cache() {
    eawr::sim::InlineExecutor executor;
    auto config = setup(); config.fleets.clear();
    auto game = take(gc::GalacticSession::create(config));
    (void)take(game.step(executor)); // Settle own/allied discovery before the idle budget.
    const auto retained = game.snapshot();
    const auto retained_values = std::vector<gc::ObservedPlanet>(retained->observations().begin(), retained->observations().end());
    for (int i = 0; i < 8; ++i) {
        game.scramble_storage_for_testing();
        const auto tick = take(game.step(executor));
        expect(tick.work.observer_projection_records == 0 && tick.snapshot->observations().data() == retained->observations().data(),
            "idle observer publications reuse the immutable projection across storage permutations");
        expect(tick.snapshot->black_market_publication() == retained->black_market_publication(),
            "idle market publications reuse the immutable vector across ticks");
    }
    require(game.apply_session_controls({}));
    expect(game.snapshot()->observations().data() == retained->observations().data(),
        "nonobserver campaign-boundary publication shares the existing projection");
    const auto parity = [](const gc::GalacticSession& session) {
        const auto snap = session.snapshot();
        const auto expected = gc::observed_planets(*snap->observer(), snap->fleets(), {}, snap->movement());
        expect(std::ranges::equal(snap->observations(), expected), "cached projection equals fresh observer evaluation");
    };
    auto command = effect(gc::ObserverAction::add, 100);
    submit(game, 1, command);
    const auto changed = take(game.step(executor));
    expect(changed.work.observer_projection_records == config.observer->records.size()
        && changed.snapshot->observations().data() != retained->observations().data()
        && view(game, 3).knowledge.fleet_contents, "source insertion rebuilds one complete projection at publication");
    parity(game);
    expect(std::ranges::equal(retained->observations(), retained_values), "previous snapshot observations remain immutable after a source commit");
    command.action = gc::ObserverAction::remove; submit(game, 2, command);
    expect(take(game.step(executor)).work.observer_projection_records == config.observer->records.size(),
        "source removal invalidates the projection");
    parity(game);
    command.action = gc::ObserverAction::add; submit(game, 3, command); (void)take(game.step(executor));
    gc::ObserverCommand refresh; refresh.action = gc::ObserverAction::refresh; refresh.player = 1; refresh.planet = 4; refresh.gameplay_frame = 131;
    submit(game, 4, refresh);
    expect(take(game.step(executor)).work.observer_projection_records == config.observer->records.size()
        && !view(game, 3).knowledge.fleet_contents, "explicit source expiry invalidates derived intelligence");
    parity(game);

    config = setup();
    // AI observers bypass ordinary discovery; isolate live presence invalidation.
    for (auto& player : config.observer->players) player.ai = true;
    auto moving = take(gc::GalacticSession::create(config));
    const auto orbit = moving.snapshot();
    require(moving.submit({{0, {1}, 1}, gc::DebugSetFleetOrbit{10, 4}}));
    const auto moved = take(moving.step(executor));
    expect(moved.work.observer_projection_records == config.observer->records.size()
        && moved.snapshot->observations().data() != orbit->observations().data(), "fleet orbit commit invalidates presence-derived observations");
    parity(moving);
    require(moving.apply_battle_losses(std::array<eawr::sim::EntityId, 1>{11}));
    expect(moving.snapshot()->observations().data() != moved.snapshot->observations().data(),
        "battle member loss invalidates presence without an ordinary galaxy step");
    parity(moving);
    expect(take(moving.step(executor)).work.observer_projection_records == 0,
        "publication after already-consumed battle loss reuses the projection");

    config = setup(); config.fleets[0].orbit = 1;
    for (auto& player : config.observer->players) player.ai = true;
    config.fleets[0].motion.facing = {f(1), {}, {}};
    config.rules.maximum_fleet_movement_distance = f(300);
    config.fleet_control.emplace(); config.fleet_control->types = {{100, false, true, false}};
    config.movement.emplace(); auto& movement = *config.movement;
    movement.mode_modifier = f(1); movement.types = {{100, take(Fixed::from_decimal("0.1")), f(1), 0, true, false, false, false}};
    for (const auto& player : config.players) movement.players.push_back({player.player_id, f(1), {}, false});
    for (const auto& planet : config.planets) movement.planets.push_back({planet.id, {}, false});
    for (const auto& player : config.players) for (const auto& planet : config.planets)
        movement.access.push_back({player.player_id, planet.id, true, false});
    auto transit = take(gc::GalacticSession::create(config));
    require(transit.submit({{0, {1}, 1}, gc::MoveFleet{10, {1, 2}, {}}}));
    expect(take(transit.step(executor)).work.observer_projection_records == config.observer->records.size()
        && transit.snapshot()->fleets()[0].motion.transit.has_value(), "departure changes orbital presence and rebuilds observations");
    parity(transit);
    const auto departed = transit.snapshot();
    for (int i = 0; i < 8; ++i) {
        const auto tick = take(transit.step(executor));
        expect(tick.work.observer_projection_records == 0 && tick.snapshot->observations().data() == departed->observations().data(),
            "pure transit-position updates reuse the orbital projection");
        parity(transit);
    }
    bool arrived{};
    for (int i = 0; i < 500 && !arrived; ++i) {
        const auto tick = take(transit.step(executor));
        if (!tick.snapshot->fleets()[0].motion.transit) {
            arrived = true;
            expect(tick.work.observer_projection_records == config.observer->records.size(), "arrival invalidates orbital observations");
        }
        parity(transit);
    }
    expect(arrived, "cache movement fixture reaches its destination");

    config.observer->types.push_back({101, false, false, true});
    config.movement->types.push_back({101, f(1), f(1), 0, true, false, false, false});
    config.movement->types[0].fleet_container = true;
    config.movement->contents = {{11, {{12, 101}}}};
    auto nested = take(gc::GalacticSession::create(config));
    const auto with_child = nested.snapshot();
    expect(with_child->observations()[0].knowledge.fleet_count, "nested live superweapon contributes orbital intelligence");
    require(nested.apply_battle_losses(std::array<eawr::sim::EntityId, 1>{12}));
    expect(nested.snapshot()->observations().data() != with_child->observations().data()
        && !nested.snapshot()->observations()[0].knowledge.fleet_count, "nested casualty updates shared movement contents and invalidates presence projection");
    parity(nested);
}

void replay_checkpoint() {
    auto config = setup(); auto c = effect(gc::ObserverAction::add, 0); c.duration = f(-1);
    config.control.emplace(); auto& control = *config.control;
    control.change_seconds = f(7); control.max_ground_forces = 10;
    control.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& a : control.players) for (const auto& b : control.players)
        control.relations.push_back({a.player, b.player, a.player == b.player});
    control.types = {{100, 1, false, false}};
    for (const auto& planet : config.planets) {
        gc::ControlPlanet row; row.planet = planet.id; row.type = 100 + planet.id;
        row.surface_accessible = true; control.planets.push_back(row);
    }
    control.routes = {{5, f(1)}};
    gc::GalacticReplay replay{config, 20, {{{0, {3}, 1}, c}}};
    const auto bytes = take(gc::write_replay(replay)); auto decoded = take(gc::parse_replay(bytes));
    expect(decoded == replay, "control and observer extensions round trip in ascending tag order");
    auto truncated = bytes; truncated.pop_back(); expect(!gc::parse_replay(truncated), "truncated observer replay rejected");
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(gc::GalacticSession::from_replay(decoded));
        std::vector<std::string> hashes;
        for (int i = 0; i < 20; ++i) {
            if (i % 3 == 0) game.scramble_storage_for_testing();
            const auto tick = take(game.step(executor)); expect(tick.state_sha256 == game.state_sha256(), "cached and from-scratch sparse section hash agree");
            expect(std::ranges::equal(tick.snapshot->observations(), gc::observed_planets(*tick.snapshot->observer(), tick.snapshot->fleets(), {}, tick.snapshot->movement())),
                "replay projection agrees with fresh computation on each worker count");
            hashes.push_back(tick.state_sha256 + game.snapshot()->sha256());
        }
        if (baseline.empty()) baseline = hashes; else expect(hashes == baseline, "observer corpus identical on 1/2/4/8 workers and scrambled storage");
    }
    eawr::sim::InlineExecutor executor;
    auto session = take(eawr::campaign::GameSession::create(config));
    require(session.submit({{0, {3}, 1}, c})); for (int i = 0; i < 9; ++i) (void)take(session.step(executor));
    eawr::checkpoint::Identities identities;
    identities.content = config.content_identity; identities.campaign = config.campaign_identity;
    const auto save = take(eawr::checkpoint::save_game_checkpoint(session, identities));
    auto restored = take(eawr::checkpoint::load_game_checkpoint(save, identities, {}, executor));
    expect(restored.session.state_sha256() == session.state_sha256(), "checkpoint retains source records and discovery counter");
    for (int i = 0; i < 12; ++i) {
        (void)take(restored.session.step(executor)); (void)take(session.step(executor));
        expect(restored.session.state_sha256() == session.state_sha256(), "checkpoint continuation remains exact through discovery delay");
    }
    config = setup(); config.observer->records[3].discovered = true;
    config.observer->records[3].apparent_owner = 2; config.observer->records[3].apparent_control = f(5);
    config.observer->records[7].mask = gc::bit(gc::Intelligence::income);
    config.observer->records[7].sources = {{1, 42, "retained nonlocal load source", gc::bit(gc::Intelligence::income), 0}};
    auto memory = take(eawr::campaign::GameSession::create(config)); (void)take(memory.step(executor));
    const auto memory_save = take(eawr::checkpoint::save_game_checkpoint(memory, identities));
    auto exact = take(eawr::checkpoint::load_game_checkpoint(memory_save, identities, {}, executor));
    expect(exact.session.galaxy().snapshot()->observer()->records[3].apparent_owner == 2, "resync restores apparent memory exactly");
    eawr::checkpoint::GameLoadOptions options; options.player_load = true;
    auto player = take(eawr::checkpoint::load_game_checkpoint(memory_save, identities, options, executor));
    const auto& loaded = player.session.galaxy().snapshot()->observer()->records[3];
    expect(loaded.discovered && loaded.apparent_owner == -1 && loaded.apparent_control == Fixed{},
        "WGUI-102 player-load ordinary refresh clears apparent memory but retains restored discovery before fix-up");
    expect(player.session.galaxy().snapshot()->observer()->records[7] == memory.galaxy().snapshot()->observer()->records[7],
        "WGUI-102 load manager refreshes local visibility without recomputing a nonlocal modifier list");
    const auto resave = take(eawr::checkpoint::save_game_checkpoint(player.session, identities));
    expect(take(eawr::checkpoint::load_game_checkpoint(resave, identities, {}, executor)).session.state_sha256() == player.session.state_sha256(),
        "player-load refresh fix-up is durable through another exact checkpoint reconstruction");
}
void discovery_audio_publication() {
    std::vector<gc::GalacticEvent> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto s = setup(); s.observer->planets[0].automatic_reveal = true;
        auto game = take(gc::GalacticSession::create(s));
        expect(game.snapshot()->events().empty(), "WGUI-05 initialization does not announce discovery");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<gc::GalacticEvent> publications;
        for (int pass = 0; pass < 22; ++pass) {
            const auto tick = take(game.step(executor));
            std::set<gc::PlayerId> discovered, feedback;
            for (const auto& event : tick.snapshot->events()) {
                if (event.kind == gc::GalacticEventKind::planet_discovered) discovered.insert(static_cast<gc::PlayerId>(event.value));
                if (event.kind == gc::GalacticEventKind::discovery_feedback) {
                    expect(feedback.insert(static_cast<gc::PlayerId>(event.value)).second,
                        "WGUI-05 each player's reveal feedback publishes once per service");
                    publications.push_back(event);
                }
            }
            expect(pass == 0 ? feedback.empty() : feedback == discovered,
                "WGUI-05 positive service feedback matches discovery; zero service suppresses sound");
        }
        expect(!publications.empty(), "discovery fixture exercises the reveal producer");
        if (workers == 1) reference = publications;
        else expect(publications == reference, "reveal publications equal on 1/2/4/8 workers");
    }
}
void black_market_publication() {
    gc::EconomyCorruptionState state;
    gc::CorruptionEconomyPlayer human; human.player = 1; human.human = true;
    state.players.push_back(human);
    gc::BlackMarketBuyer buyer; buyer.buyer = 10; buyer.owner = 1;
    state.buyers.push_back(buyer); buyer.buyer = 11; buyer.owner = 2; state.buyers.push_back(buyer);
    gc::corruption_fixture::MarketItem item; item.id = 7; item.tech = 3; state.items.push_back(item);
    const auto price = take(Fixed::from_decimal("100.4"));
    state.menus = {{10, 4, {{99, f(1), false}, {7, price, false}, {7, f(1), true}}, f(60), true},
        {11, 4, {{7, f(1), false}}, {}, true}};
    gc::PlayerEconomySetup accounts;
    gc::GalacticPlayerEconomy account; account.player = 1; account.credits = take(Fixed::from_decimal("100.3"));
    accounts.players.push_back(account);
    gc::ObservedPlanet observation; observation.player = 1; observation.planet = 4; observation.discovered = true;
    const auto rows = gc::observed_black_market_menus(state, accounts, 1, std::span(&observation, 1));
    expect(rows.size() == 1 && rows[0].buyer == 10, "market filters foreign buyer menus");
    expect(rows[0].offers.size() == 2 && rows[0].offers[0].index == 1, "unresolved item retains original vector index");
    expect(rows[0].offers[0].affordable && rows[0].offers[0].price == price,
        "rounded affordability retains unaffordable exact price");
    expect(rows[0].offers[1].owned && !rows[0].offers[1].affordable, "purchased item stays disabled");
    observation.discovered = false;
    expect(gc::observed_black_market_menus(state, accounts, 1, std::span(&observation, 1)).empty(), "hidden planet menu denied");
    observation.discovered = true; state.players[0].human = false;
    expect(gc::observed_black_market_menus(state, accounts, 1, std::span(&observation, 1)).empty(), "AI menu denied");
    state.players[0].human = true;
    accounts.players[0].credits = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    const auto overflow = gc::observed_black_market_menus(state, accounts, 1, std::span(&observation, 1));
    expect(!overflow[0].offers[0].affordability_available, "rounding overflow is unavailable, not a known denial");
    gc::ObserverSetup observer; observer.human_observers = {1}; observer.solo_reference_human = 1;
    const auto snapshot = [&](bool suppress_derived) {
        return std::make_shared<const gc::GalacticSnapshot>(0, gc::GalacticClock{}, std::vector<gc::GalacticPlayer>{},
            std::vector<gc::Planet>{}, std::vector<gc::Route>{}, std::vector<gc::Fleet>{}, std::vector<gc::GalacticEvent>{},
            std::nullopt, std::nullopt, nullptr, std::nullopt, std::vector<gc::Seat>{}, accounts, std::nullopt,
            std::nullopt, std::vector<gc::PoliticalControlEffect>{}, std::vector<gc::PopulationTotal>{}, observer,
            std::nullopt, std::nullopt, std::vector<gc::ObservedProductionOffer>{},
            std::make_shared<const std::vector<gc::ObservedPlanet>>(std::vector{observation}), std::nullopt, nullptr,
            std::nullopt, std::nullopt, state, nullptr, 0, nullptr, nullptr,
            suppress_derived ? std::make_shared<const std::vector<gc::ObservedBlackMarketMenu>>() : nullptr);
    };
    const auto published = snapshot(false), omitted = snapshot(true);
    expect(!published->black_market_menus().empty() && omitted->black_market_menus().empty(), "canonical comparison changes only derived view");
    expect(published->canonical_bytes() == omitted->canonical_bytes(), "derived market publication never enters canonical bytes");
}
void planet_spy_lifecycle() {
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const auto range : {0, 10}) {
        auto config = setup(); config.fleets.clear(); config.observer->records.clear();
        const auto mask = gc::bit(gc::Intelligence::income_breakdown);
        config.observer->planet_spies = {
            {1, 1, "renamed network", mask, f(30), f(range), gc::SpyStyle::galactic_automatic, true, false},
            {1, 2, "independent network", gc::bit(gc::Intelligence::major_stealth), f(-1), f(range), gc::SpyStyle::galactic_automatic, true, false},
            {2, 1, "overlapping other planet", mask, f(-1), f(10), gc::SpyStyle::galactic_automatic, true, false}};
        require(gc::initialize_campaign_observer(config, {}));
        auto game = take(gc::GalacticSession::create(config));
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto contains = [&](gc::PlayerId player, gc::PlanetId planet, eawr::sim::EntityId source, std::uint64_t ability) {
            const auto& records = game.snapshot()->observer()->records;
            const auto at = std::find_if(records.begin(), records.end(), [&](const auto& row) { return row.player == player && row.planet == planet; });
            return at != records.end() && std::any_of(at->sources.begin(), at->sources.end(),
                [&](const auto& row) { return row.source == source && row.ability == ability; });
        };
        expect(contains(1, 1, 1, 1) && contains(1, 3, 1, 1) == (range > 0)
            && !contains(1, 2, 1, 1) && !contains(1, 4, 1, 1), "planet spy uses authored local/strict nearby range, including zero range");
        expect(!view(game, 2).discovered && view(game, 2).knowledge.income_breakdown == (range > 0),
            "published intelligence follows initial influence without replacing discovery");
        require(game.submit({{0, {1}, 1}, gc::DebugSetPlanetOwner{1, 2}}));
        (void)take(game.step(executor));
        expect(!contains(1, 1, 1, 1) && contains(2, 1, 1, 1) && contains(2, 3, 1, 1) == (range > 0)
            && contains(2, 3, 2, 1), "acquisition transfers only the planet source and preserves another overlapping source");
        require(game.submit({{1, {1}, 2}, gc::DebugSetPlanetOwner{1, 3}}));
        (void)take(game.step(executor));
        expect(!contains(2, 1, 1, 1) && contains(3, 1, 1, 1) && contains(2, 3, 2, 1), "capture removes old recipient and adds current owner immediately");
        auto termination = effect(gc::ObserverAction::terminate, 2);
        termination.source = {1, 1, "renamed network", mask, 0}; termination.player = 3;
        submit(game, 1, termination); (void)take(game.step(executor));
        expect(!contains(3, 1, 1, 1) && contains(3, 1, 1, 2), "termination removes one ability and retains an independent ability on the same source");
        require(game.submit({{3, {1}, 3}, gc::DebugSetPlanetOwner{1, 1}}));
        (void)take(game.step(executor));
        expect(!contains(1, 1, 1, 1) && contains(1, 1, 1, 2), "terminated ability cannot restart on later ownership transfer");
        gc::GalacticReplay replay{config, 4, {
            {{0, {1}, 1}, gc::DebugSetPlanetOwner{1, 2}}, {{1, {1}, 2}, gc::DebugSetPlanetOwner{1, 3}},
            {{2, {3}, 1}, termination}, {{3, {1}, 3}, gc::DebugSetPlanetOwner{1, 1}}}};
        auto decoded = take(gc::parse_replay(take(gc::write_replay(replay))));
        expect(decoded == replay, "planet spy definitions and registered lists round trip through replay");
        auto replayed = take(gc::GalacticSession::from_replay(decoded));
        for (int tick = 0; tick < 4; ++tick) (void)take(replayed.step(executor));
        expect(replayed.state_sha256() == game.state_sha256(), "replay reproduces automatic source registration and ownership callbacks");
        auto campaign = take(eawr::campaign::GameSession::create(config));
        eawr::checkpoint::Identities identities;
        const auto bytes = take(eawr::checkpoint::save_game_checkpoint(campaign, identities));
        auto restored = take(eawr::checkpoint::load_game_checkpoint(bytes, identities, {}, executor));
        expect(campaign.state_sha256() == restored.session.state_sha256(), "checkpoint retains planet spy definitions and modifiers without duplicate insertion");
        const std::vector<std::string> hashes{game.state_sha256(), restored.session.state_sha256()};
        const auto index = static_cast<std::size_t>(range > 0) * 2;
        if (workers == 1) baseline.insert(baseline.end(), hashes.begin(), hashes.end());
        else expect(hashes[0] == baseline[index] && hashes[1] == baseline[index + 1], "planet spy lifecycle equal on 1/2/4/8 workers");
    }
}
void planet_spy_non_source_budget() {
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto config = setup(); config.fleets.clear(); config.observer->records.clear();
        config.observer->planet_spies = {{1, 1, "active network", gc::bit(gc::Intelligence::income),
            f(-1), f(10), gc::SpyStyle::galactic_automatic, true, false}};
        require(gc::initialize_campaign_observer(config, {}));
        for (auto& record : config.observer->records) record.discovered = true;
        auto game = take(gc::GalacticSession::create(config));
        const auto observations = game.snapshot()->observations().data();
        require(game.submit({{0, {1}, 1}, gc::DebugSetPlanetOwner{4, 1}}));
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto tick = take(game.step(executor));
        expect(tick.work.observer_projection_records == 0 && tick.snapshot->observations().data() == observations,
            "non-spy ownership events do not invalidate and recompute the observer projection");
    }
}
void capture_metrics_contract() {
    eawr::viewer::galactic::CampaignCaptureMetrics::Player activity;
    activity.activity(90); activity.activity(30);
    expect(activity.last_activity_frame == 90 && activity.maximum_idle_frames == 90,
        "out-of-order activity does not underflow or rewind the capture idle clock");
    auto config = setup();
    for (auto& seat : config.seats) { seat.kind = gc::SeatKind::ai; seat.human_side_id = 0; }
    gc::PlayerEconomySetup economy;
    economy.players.push_back({1, f(100), f(1000), 1, 3}); config.player_economy = economy;
    gc::ProductionSetup production;
    gc::PlanetProduction planet; planet.planet = 1; planet.target_owner = 1;
    gc::ProductionOffer offer; offer.type = 77; offer.domain = gc::ProductionDomain::space;
    offer.kind = gc::ProductionKind::unit; planet.offers.push_back(offer);
    production.planets.push_back(planet); config.production = production;
    const auto publication = [&](std::uint64_t frame) {
        gc::GalacticClock clock; clock.galactic_frame = clock.session_tick = frame;
        eawr::platform::LiveGameFrame result; result.session_tick = frame;
        result.latest = std::make_shared<gc::GalacticSnapshot>(frame, clock, config.players, config.planets,
            config.routes, config.fleets, std::vector<gc::GalacticEvent>{}, std::nullopt, std::nullopt,
            nullptr, std::nullopt, config.seats, config.player_economy, config.production);
        return result;
    };
    eawr::viewer::galactic::CampaignCaptureMetrics metrics;
    metrics.observe(publication(0), {});
    gc::ProductionEntry entry; entry.type = 77; entry.builder = 1; entry.build_id = 900;
    config.production->planets[0].queues[1].push_back(entry);
    eawr::platform::LiveGalacticEvents feed; feed.through = 3;
    feed.events = {{1, {1, gc::GalacticEventKind::command_executed, {1}, 10, 1, 4}, std::nullopt},
        {2, {1, gc::GalacticEventKind::movement_begin, {1}, 10, 1, 4}, std::nullopt},
        {3, {1, gc::GalacticEventKind::production_started, {}, 1, 900, 77}, std::nullopt}};
    metrics.observe(publication(1), feed);
    config.production->planets[0].queues[1].clear(); config.planets[3].owner = 1;
    config.production->planets[0].offers.clear();
    feed.through = 6; feed.events = {
        {4, {1800, gc::GalacticEventKind::production_completed, {}, 1, 900, 77}, std::nullopt},
        {5, {1800, gc::GalacticEventKind::planet_owner_changed, {}, 4, 0, 1}, 3},
        {6, {1800, gc::GalacticEventKind::conflict_pending, {}, 99, 0, 1}, std::nullopt}};
    feed.events.back().pending_alert = eawr::platform::LiveGalacticEvent::PendingAlert{1, 2, false};
    auto completed = publication(1800);
    auto outcome = std::make_shared<eawr::platform::LiveAutoResolveOutcome>();
    outcome->invader = 1; outcome->defender = 2; outcome->result.conflict = 99; outcome->result.winner = 1;
    completed.auto_resolve_outcome = outcome;
    metrics.observe(completed, feed);
    metrics.observe(completed, {6, false, {}}); // retained outcome is not a second completed battle
    auto idle = publication(3600); metrics.observe(idle, {6, false, {}});
    const auto report = metrics.json();
    expect(metrics.failure.empty() && report.find("\"all_ai\":true") != std::string::npos, "capture collector admits all-AI published seats");
    for (const auto value : {"\"fleet_moves\":1", "\"builds_completed\":1", "\"units_built_space\":1",
        "\"attacks\":1", "\"battles_won\":1", "\"battles_lost\":1", "\"planets_taken_beyond_start\":1",
        "\"maximum_idle_frames\":1800", "\"hostile_moves\":1", "\"conflicts_started\":1",
        "\"production_started_by_category\":{\"space\":1,\"ground\":0,\"structures\":0,\"starbase_upgrades\":0",
        "\"production_completed_by_category\":{\"space\":1,\"ground\":0", "\"unavailable_perceptions\":null"}) expect(report.find(value) != std::string::npos, value);
    expect(report.find("\"fleet_moves\":2") == std::string::npos && report.find("\"battles\":2") == std::string::npos,
        "move requests and repeated retained outcomes do not inflate committed metrics");
    metrics.observe(idle, {6, true, {}});
    expect(!metrics.failure.empty(), "event retention gaps refuse a complete capture report");

    // Missing admission classification is explicitly unavailable, never a known zero.
    eawr::viewer::galactic::CampaignCaptureMetrics unknown;
    unknown.observe(publication(0), {});
    config.production->planets[0].queues[1].push_back(entry);
    unknown.observe(publication(1), {1, false, {{1,
        {1, gc::GalacticEventKind::production_started, {}, 1, 900, 77}, std::nullopt}}});
    config.production->planets[0].queues[1].clear();
    unknown.observe(publication(1800), {2, false, {{2,
        {1800, gc::GalacticEventKind::production_completed, {}, 1, 900, 77}, std::nullopt}}});
    expect(unknown.failure.empty() && unknown.json().find("\"builds_unclassified\":1,\"units_built_available\":false") != std::string::npos,
        "unclassified completed purchases mark the unit count unavailable");

    // Every production category is counted on its own event, ground channel included.
    {
        config.production->planets[0].offers.clear();
        const std::vector<std::pair<std::uint64_t, gc::ProductionKind>> kinds{{77, gc::ProductionKind::unit},
            {78, gc::ProductionKind::unit}, {79, gc::ProductionKind::structure},
            {80, gc::ProductionKind::starbase}, {81, gc::ProductionKind::technology}};
        for (const auto& [type, kind] : kinds) {
            gc::ProductionOffer o; o.type = type; o.kind = kind; config.production->planets[0].offers.push_back(o);
        }
        eawr::viewer::galactic::CampaignCaptureMetrics categories;
        categories.observe(publication(0), {});
        eawr::platform::LiveGalacticEvents started, done; started.through = 5; done.through = 10;
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            gc::ProductionEntry e; e.type = kinds[i].first; e.builder = 1; e.build_id = 910 + i;
            config.production->planets[0].queues[i == 0 ? 1 : 0].push_back(e);
            started.events.push_back({i + 1, {1, gc::GalacticEventKind::production_started, {}, 1, 910 + i, kinds[i].first}, std::nullopt});
            done.events.push_back({i + 6, {2, gc::GalacticEventKind::production_completed, {}, 1, 910 + i, kinds[i].first}, std::nullopt});
        }
        categories.observe(publication(1), started);
        categories.observe(publication(2), done);
        categories.observe(publication(1800), {10, false, {}}); // checkpoint publishes the counters
        const auto text = categories.json();
        const std::string each = "{\"space\":1,\"ground\":1,\"structures\":1,\"starbase_upgrades\":1,\"technology\":1}";
        expect(categories.failure.empty() && text.find("\"production_started_by_category\":" + each) != std::string::npos
            && text.find("\"production_completed_by_category\":" + each) != std::string::npos,
            "production categories are counted for started and completed events");
    }

    // A move into a neutral-owned planet is expansion, not a hostile move.
    {
        eawr::viewer::galactic::CampaignCaptureMetrics neutral;
        neutral.neutral_owner = 3;
        neutral.observe(publication(0), {});
        neutral.observe(publication(1), {2, false, {
            {1, {1, gc::GalacticEventKind::movement_begin, {1}, 10, 1, 3}, std::nullopt},
            {2, {1, gc::GalacticEventKind::movement_begin, {1}, 10, 2, 2}, std::nullopt}}});
        neutral.observe(publication(1800), {2, false, {}}); // checkpoint publishes the counters
        const auto text = neutral.json();
        expect(neutral.failure.empty() && text.find("\"fleet_moves\":2,\"hostile_moves\":1") != std::string::npos,
            "moves into neutral planets are not hostile");
    }

    // A publication that jumps over a 60 s boundary still gets its checkpoint.
    {
        eawr::viewer::galactic::CampaignCaptureMetrics jump;
        const auto samples = [&] {
            std::size_t count = 0;
            for (auto at = jump.json().find("\"galactic_frame\""); at != std::string::npos; at = jump.json().find("\"galactic_frame\"", at + 1)) ++count;
            return count;
        };
        jump.observe(publication(0), {});
        jump.observe(publication(1790), {});
        expect(samples() == 1, "no checkpoint before the first 60 s boundary");
        jump.observe(publication(1810), {});
        expect(samples() == 2, "a publication that skips the 60 s boundary is sampled once past it");
        jump.observe(publication(1815), {});
        expect(samples() == 2, "the next checkpoint waits for the next 60 s boundary");
    }
}
} // namespace

int case_galactic_observer_tests() {
    if (!galactic_support_self_test()) return 1;
    black_market_publication();
    discovery_audio_publication();
    try { solo_observer_publication(); multiplayer_observer_contexts(); capture_metrics_contract(); planet_spy_non_source_budget(); planet_spy_lifecycle(); predicates(); discovery(); campaign_home_expansion(); arrival_order_and_budget(); landed_ownership(); effects(); staggered_observer_sources(); apparent_memory(); source_lifecycle(); projection_cache(); replay_checkpoint(); }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

