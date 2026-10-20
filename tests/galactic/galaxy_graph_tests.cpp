#include <algorithm>
#include <cstdint>
#include <ios>
#include <vector>
#include <iostream>
#include <cstddef>
#include <string_view>
#include "../support/env.hpp"
#include "galactic_support.hpp"

#include "eawr/campaign/galaxy.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/vfs/vfs.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace {
namespace gc = eawr::sim::galactic;
using eawr::sim::math::Fixed;
using galactic_test::expect;
Fixed f(const std::int64_t n) { return Fixed::from_integer(n).value(); }
gc::Planet p(const gc::PlanetId id, const std::int64_t x, const std::int64_t y = 0) { return {id, {f(x), f(y), {}}, 1}; }
std::vector<gc::PathPlanet> observations(const gc::PlanetGraph& graph) {
    std::vector<gc::PathPlanet> result;
    for (const auto& node : graph.nodes()) { gc::PathPlanet row; row.id = node.id; row.ai_enemy_space_force = false; result.push_back(row); }
    return result;
}
void paths() {
    const std::vector planets{p(1, 0), p(2, 3, 4), p(3, 3, -4), p(4, 6), p(5, 200)};
    const std::vector<gc::Route> routes{{6, 4, 5, f(2)}};
    auto built = gc::PlanetGraph::build(planets, routes, f(5));
    expect(static_cast<bool>(built), "graph builds"); if (!built) return;
    const auto& graph = built.value(); auto access = observations(graph);
    expect(graph.work() == gc::GraphWork{10, 10}, "pair and directed-edge budgets");
    expect(graph.node(1)->edges.size() == 2 && graph.node(5)->edges.size() == 1, "inclusive proximity threshold and long trade route");
    expect(graph.node(5)->edges.front().cost == f(97), "WGM-17 route cost divides distance");
    auto path = gc::find_planet_path(graph, 1, 5, access).value();
    expect(path.planets == std::vector<gc::PlanetId>{1, 2, 4, 5} && path.cost == f(107), "equal alternatives retain ascending-ID predecessor");
    expect(path.work == gc::PathWork{4, 9}, "search deterministic work budget");
    expect(gc::find_planet_path(graph, 5, 1, access).value().planets == std::vector<gc::PlanetId>{5, 4, 2, 1}, "connections both ways");
    expect(gc::find_planet_path(graph, 1, 1, {}).value().planets == std::vector<gc::PlanetId>{1}, "same endpoint one-planet path before config");
    expect(!gc::find_planet_path(graph, 0, 1, access), "unknown endpoint rejects");
    expect(!gc::find_planet_path(graph, 1, 5, {}), "missing observer configuration rejects");
    access[1].revealed = access[2].revealed = false;
    expect(gc::find_planet_path(graph, 1, 5, access).value().planets.empty(), "single-player hidden hops unreachable");
    gc::PathOptions options; options.multiplayer = true;
    expect(!gc::find_planet_path(graph, 1, 5, access, options).value().planets.empty(), "multiplayer omits reveal gate");
    options.ai_player = true; access[0].ai_usable = false;
    expect(gc::find_planet_path(graph, 1, 5, access, options).value().planets.empty(), "AI restriction validates current node");
    options.ai_restrictions = false;
    expect(!gc::find_planet_path(graph, 1, 5, access, options).value().planets.empty(), "AI restriction flag disables endpoint checks");
    auto reversed = planets; std::reverse(reversed.begin(), reversed.end());
    expect(!gc::PlanetGraph::build(planets, {}, f(5)).value().largest_route_factor(), "route-less graph has no invented route factor");
    const auto reordered = gc::PlanetGraph::build(reversed, routes, f(5)).value();
    expect(std::equal(graph.nodes().begin(), graph.nodes().end(), reordered.nodes().begin(), reordered.nodes().end()), "storage order does not affect graph");
    auto invalid_routes = routes; invalid_routes.push_back({7, 5, 4, f(3)});
    expect(!gc::PlanetGraph::build(planets, invalid_routes, f(5)), "unverified duplicate lane selection diagnosed");
    invalid_routes = {{6, 4, 5, {}}};
    expect(!gc::PlanetGraph::build(planets, invalid_routes, f(5)), "nonpositive route factor rejected");
    invalid_routes = {{0, 4, 5, f(2)}};
    expect(!gc::PlanetGraph::build(planets, invalid_routes, f(5)), "route requires stable nonzero identity");
    invalid_routes = {{1, 4, 5, f(2)}};
    expect(!gc::PlanetGraph::build(planets, invalid_routes, f(5)), "route identity cannot collide with a planet");
    auto overflow = planets; overflow[0].position.x = Fixed::from_raw(INT64_MIN); overflow[1].position.x = Fixed::from_raw(INT64_MAX);
    expect(!gc::PlanetGraph::build(overflow, routes, f(5)), "position subtraction overflow diagnosed");
    const std::vector disconnected{p(1, 0), p(2, 100)};
    const auto isolated = gc::PlanetGraph::build(disconnected, {}, f(5)).value();
    expect(gc::find_planet_path(isolated, 1, 2, observations(isolated)).value().planets.empty(), "disconnected target unreachable");
}
void gates() {
    const std::vector planets{p(1, 0), p(2, 10), p(3, 20)};
    const auto graph = gc::PlanetGraph::build(planets, {}, f(10)).value();
    auto access = observations(graph); gc::PathOptions options;
    const auto path = [&] { return gc::find_planet_path(graph, 1, 3, access, options).value(); };
    options.allow_bad_if_no_force = false; access[1].relation = gc::PathRelation::hostile;
    expect(path().planets.empty(), "hostile intermediate rejects");
    options.allow_hostile = true; expect(path().cost == f(20), "hostile passability accepts undefended");
    options.allow_hostile = false; access[1].owner_visible = false;
    expect(path().planets.empty(), "invisible owner classified neutral");
    options.allow_neutral = true; expect(path().cost == f(20), "neutral passability independent of hostile");
    options = {}; access[1].owner_visible = true;
    expect(path().cost == f(20), "bad faction without force allowed by default");
    access[1].fleet_presence_visible = true; access[1].nonallied_fleet = true;
    expect(path().planets.empty(), "visible nonallied attached fleet defends path");
    access[1].starbase_relation = gc::PathRelation::allied;
    expect(path().cost == f(20), "allied starbase supersedes fleet defense");
    access[1].starbase_relation = gc::PathRelation::hostile;
    options.benefits_from_corruption = true; access[1].corrupted = true;
    expect(path().cost == f(120), "needed corruption bypass costs 100 per hop");
    options.allow_hostile = true; options.allow_defended = true;
    expect(path().cost == f(20), "unused corruption bypass has no penalty");
    access[1].starbase_relation.reset(); access[1].nonallied_fleet = false;
    options.allow_hostile = false; options.allow_bad_if_no_force = false;
    expect(path().cost == f(20), "corruption passes allegiance before no-force flag and has no undefended penalty");
    access[1].special_space_map = true;
    expect(path().planets.empty(), "story special map blocks intermediate even with corruption");
    access = observations(graph); options = {}; options.allow_bad_if_no_force = false;
    access[2].special_space_map = true; access[2].relation = gc::PathRelation::hostile; access[2].starbase_relation = gc::PathRelation::hostile;
    expect(path().planets.empty(), "bad final rejects by default");
    options.allow_bad_final = true; expect(path().cost == f(20), "final bypass skips map allegiance and defense");
    access[2].revealed = false; expect(path().planets.empty(), "final bypass does not skip reveal");
    access = observations(graph); options = {}; options.ai_player = true; options.allow_neutral = true;
    access[1].ai_enemy_space_force.reset();
    expect(path().planets.empty(), "failed AI perception conservatively defended");
    access[1].ai_enemy_space_force = false; access[1].ai_relation = gc::PathRelation::hostile;
    access[1].relation = gc::PathRelation::hostile;
    options.allow_bad_if_no_force = false;
    expect(path().planets.empty(), "AI classification delegated");
    access[1].ai_classification_succeeded = false;
    expect(path().cost == f(20), "failed AI classification accepts allegiance");
    access[1].ai_classification_succeeded = true; access[1].ai_relation.reset(); options.allow_neutral = false;
    expect(path().planets.empty(), "missing classified player follows neutral passability");
    access = observations(graph); options = {}; options.allow_bad_if_no_force = false;
    access[1].owner_visible = false;
    expect(path().cost == f(20), "actual ally accepts before invisible-owner neutral classification");
    access[0].starbase_relation = gc::PathRelation::hostile;
    expect(path().planets.empty(), "defended source cannot expand");
    access[0].corrupted = true; options.benefits_from_corruption = true;
    expect(path().cost == f(20), "corruption permits source expansion without a hop penalty at source");
}
void campaign_boundary() {
    eawr::data::galactic::GalacticData data;
    eawr::data::galactic::Planet alpha; alpha.name = "Alpha";
    alpha.behaviors = {"PLANET"}; alpha.in_background = true;
    alpha.galactic_position = eawr::data::galactic::Position{f(0), f(0), f(0)};
    auto beta = alpha; beta.name = "Beta"; beta.galactic_position->x = f(200);
    data.planets = {alpha, beta};
    eawr::data::galactic::TradeRoute lane; lane.name = "Lane";
    lane.point_a = "alpha"; lane.point_b = "BETA"; lane.hs_speed_factor = f(4);
    data.trade_routes = {lane}; data.constants = {{"MaximumFleetMovementDistance", "5", f(5)}};
    eawr::data::galactic::Campaign campaign; campaign.name = "Test";
    campaign.planets = {"Beta", "Alpha"}; campaign.trade_routes = {"Lane"};
    auto art = alpha; art.name = "Renamed_Art"; art.behaviors = {"IDLE"};
    data.galaxy_art_objects.push_back(art); campaign.locations = {art.name, "Beta", "Alpha"};
    eawr::units::GalacticTypeTable types;
    const auto model = eawr::campaign::build_campaign_galaxy(data, types, campaign, 7, 100).value();
    expect(model.planets[0].id == 100 && model.planets[0].position.x == f(200)
        && model.planets[1].owner == 7 && model.next_id == 103, "authored planet order and supplied allocator/owner");
    expect(model.routes == std::vector<gc::Route>{{102, 101, 100, f(4)}}, "case-insensitive route endpoints and authored factors");
    expect(model.names[0].name == "Beta" && model.maximum_movement_distance == f(5), "name bindings and loaded movement limit");
    expect(model.planets.size() == 2 && model.names.size() == 3,
        "WGS-01a: background flag does not exclude planets; authored art locations stay outside graph state");
    auto invalid_campaign = campaign; invalid_campaign.planets.push_back(art.name);
    expect(!eawr::campaign::build_campaign_galaxy(data, types, invalid_campaign, 7, 100),
        "WGS-01a: art cannot be admitted as a graph planet even in a manually altered campaign");
    eawr::units::GalacticType ship; ship.name = "Ship"; ship.hyperspace_speed = f(3); types.types.push_back(ship);
    const auto changed = eawr::campaign::build_campaign_galaxy(data, types, campaign, 7, 100).value();
    expect(changed.content_identity != model.content_identity, "original-type table participates in GC content identity");
    expect(!eawr::campaign::build_campaign_galaxy(data, types, campaign, 0, 100), "caller must supply initial owner");
    data.constants.clear();
    expect(!eawr::campaign::build_campaign_galaxy(data, types, campaign, 7, 100), "missing constant does not invent stock limit");
    data.constants = {{"MaximumFleetMovementDistance", "5", f(5)}}; data.trade_routes[0].hs_speed_factor.reset();
    expect(!eawr::campaign::build_campaign_galaxy(data, types, campaign, 7, 100), "missing factor does not invent stock speed");
    data.trade_routes[0].hs_speed_factor = f(4); data.planets[0].galactic_position.reset();
    expect(!eawr::campaign::build_campaign_galaxy(data, types, campaign, 7, 100), "missing position diagnosed");
}
gc::GalacticSetup setup() {
    gc::GalacticSetup value;
    value.players = {{1, 1, 0}}; value.seats = {{gc::SeatId{1}, 1, gc::SeatKind::human, 0, true, 1}};
    value.planets = {p(1, 0), p(2, 10), p(3, 100)}; value.routes = {{4, 2, 3, f(3)}};
    value.rules.maximum_fleet_movement_distance = f(10);
    return value;
}
// WGS-182..184: renamed authored types; no dependency on stock endpoint spellings.
void external_endpoints() {
    eawr::data::galactic::GalacticData data;
    eawr::data::galactic::Planet first; first.name = "Amber"; first.behaviors = {"PLANET"};
    first.galactic_position = eawr::data::galactic::Position{f(0), f(0), f(0)};
    auto second = first; second.name = "Cobalt"; second.galactic_position->x = f(200);
    auto outside = first; outside.name = "Violet";
    auto variant = first; variant.name = "Amber_Variant";
    data.planets = {first, second, outside, variant};
    data.constants = {{"MaximumFleetMovementDistance", "5", f(5)}};
    eawr::data::galactic::TradeRoute lane; lane.name = "Renamed_Lane";
    lane.point_a = "AMBER"; lane.point_b = "cobalt"; lane.hs_speed_factor = f(4);
    data.trade_routes = {lane};
    eawr::data::galactic::Campaign campaign; campaign.name = "Renamed_Campaign";
    campaign.file = "data/xml/renamed_campaign.xml";
    campaign.planets = {first.name, second.name}; campaign.locations = campaign.planets;
    campaign.trade_routes = {lane.name};
    const eawr::units::GalacticTypeTable types;
    const auto build = [&] { return eawr::campaign::build_campaign_galaxy(data, types, campaign, 1, 1); };
    const auto control = build().value();
    expect(control.routes.size() == 1 && control.next_id == 4 && control.diagnostics.empty(),
        "complete renamed route remains unchanged");
    for (const bool missing_a : {false, true}) {
        data.trade_routes[0].point_a = missing_a ? outside.name : first.name;
        data.trade_routes[0].point_b = missing_a ? second.name : outside.name;
        const auto missing = build().value();
        expect(missing.routes.empty() && missing.names.size() == 2 && missing.next_id == 3,
            "one absent endpoint consumes no graph identity in either orientation");
        expect(missing.diagnostics.size() == 1
            && missing.diagnostics[0].code == "campaign_route_external_endpoint"
            && missing.diagnostics[0].severity == eawr::core::Severity::warning
            && missing.diagnostics[0].logical_path == campaign.file
            && missing.diagnostics[0].message.find(outside.name) != std::string::npos,
            "one absent endpoint is a named warning with campaign provenance");
        const auto graph = gc::PlanetGraph::build(missing.planets, missing.routes, missing.maximum_movement_distance).value();
        expect(graph.node(1)->edges.empty() && graph.node(2)->edges.empty(),
            "skipped route contributes no long-distance adjacency");
    }
    data.trade_routes[0].point_a = outside.name; data.trade_routes[0].point_b = variant.name;
    const auto both = build().value();
    expect(both.routes.empty() && both.diagnostics.empty() && both.next_id == 3,
        "both absent endpoint types silently contribute no route");
    data.trade_routes[0] = lane;
    campaign.planets[0] = variant.name;
    const auto variant_only = build().value();
    expect(variant_only.routes.empty() && variant_only.diagnostics.size() == 1,
        "a same-position derived type does not substitute for an absent parent endpoint");
    data.trade_routes[0].point_a = "amber_variant";
    expect(build().value().routes.size() == 1, "an explicitly named variant endpoint resolves case-insensitively");
    campaign.planets.push_back("AMBER_VARIANT");
    const auto duplicate = build().value();
    expect(duplicate.planets.size() == 3 && duplicate.routes[0].from == 3 && duplicate.routes[0].to == 2
        && duplicate.diagnostics.size() == 1 && duplicate.diagnostics[0].code == "campaign_route_duplicate_endpoint"
        && duplicate.diagnostics[0].message.find(lane.name) != std::string::npos,
        "duplicate live endpoint lookup selects first live, last-created instance and warns");
    campaign.trade_routes.clear();
    expect(build().value().diagnostics.empty(), "duplicate locations without a route lookup do not warn");
    campaign.trade_routes = {lane.name};
    campaign.planets.push_back(outside.name);
    data.trade_routes[0] = lane; data.trade_routes[0].point_a = second.name;
    data.trade_routes[0].point_b = outside.name;
    expect(build().value().diagnostics.empty(), "a route not using the duplicated type does not warn");
    campaign.planets = {first.name, second.name};
    data.trade_routes[0] = lane; data.trade_routes[0].point_b = "Unknown_Type";
    expect(!build(), "undefined endpoint type still refuses instead of becoming campaign-external tolerance");
    data.trade_routes[0] = lane; data.trade_routes[0].point_b = outside.name;
    auto valid = lane; valid.name = "Retained_Lane"; data.trade_routes.push_back(valid);
    campaign.trade_routes.push_back(valid.name);
    const auto external_only = build().value();
    expect(external_only.routes.size() == 1 && external_only.routes[0].id == 3 && external_only.names.back().name == valid.name,
        "a skipped route does not shift a following admitted route identity");
    auto repeated_pair = valid; repeated_pair.name = "Later_Lane";
    std::swap(repeated_pair.point_a, repeated_pair.point_b); repeated_pair.hs_speed_factor = f(8);
    data.trade_routes.push_back(repeated_pair); campaign.trade_routes.push_back(repeated_pair.name);
    const auto admitted = build().value();
    expect(admitted.routes == external_only.routes && admitted.next_id == external_only.next_id
        && admitted.names.back().name == valid.name && admitted.diagnostics.size() == 2
        && admitted.diagnostics.back().code == "campaign_route_duplicate_pair",
        "duplicate unordered pair keeps first authored route, factor and identities with a named warning");
    std::swap(campaign.trade_routes[1], campaign.trade_routes[2]);
    const auto reordered = build().value();
    expect(reordered.routes.size() == 1 && reordered.routes[0].hs_speed_factor == f(8)
        && reordered.names.back().name == repeated_pair.name,
        "route winner follows campaign authoring order rather than global table order");
    std::swap(campaign.trade_routes[1], campaign.trade_routes[2]);
    auto value = setup(); value.planets = admitted.planets; value.routes = admitted.routes;
    value.rules.maximum_fleet_movement_distance = admitted.maximum_movement_distance;
    value.content_identity = admitted.content_identity;
    std::vector<std::string> reference;
    std::vector<std::uint8_t> reference_checkpoint;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto session = eawr::campaign::GameSession::create(value).value();
        const auto identities = eawr::checkpoint::game_identities(value);
        const auto saved = eawr::checkpoint::save_game_checkpoint(session, identities).value();
        if (workers == 1) reference_checkpoint = saved;
        else expect(saved == reference_checkpoint, "external-route checkpoint bytes equal at 1/2/4/8 workers");
        eawr::checkpoint::GameLoadOptions options;
        auto restored = eawr::checkpoint::load_game_checkpoint(saved, identities, options, executor);
        expect(static_cast<bool>(restored), "external-route graph checkpoint restores");
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 3; ++tick) {
            hashes.push_back(session.step(executor).value().state_hash.get());
            if (restored) expect(restored.value().session.step(executor).value().state_hash.get() == hashes.back(),
                "external-route restored continuation equals uninterrupted state");
        }
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "external-route graph/checkpoint identity equals at 1/2/4/8 workers");
    }
    const auto before = admitted.content_identity;
    data.trade_routes[0].point_b = variant.name;
    expect(build().value().content_identity != before, "skipped authored route definitions remain in content identity");
}
void persistence() {
    const auto value = setup();
    const auto bytes = gc::write_replay({value, 8, {}}).value();
    expect(gc::parse_replay(bytes).value().setup == value, "graph rules replay round trip");
    auto unknown = bytes; unknown[100] = 2;
    expect(!gc::parse_replay(unknown), "unknown graph header extension refused");
    auto truncated = bytes; truncated.resize(110);
    expect(!gc::parse_replay(truncated), "truncated graph rules refused");
    auto wrong_id = bytes; wrong_id[118] = 99;
    expect(!gc::parse_replay(wrong_id), "factor IDs must match routes");
    auto changed = value; changed.routes[0].hs_speed_factor = f(4);
    auto original = gc::GalacticSession::create(value).value(); auto other = gc::GalacticSession::create(changed).value();
    expect(original.state_sha256() != other.state_sha256(), "route factor participates in graph hash section");
    expect(original.snapshot()->sha256() != other.snapshot()->sha256(), "route factor participates in snapshot bytes");
    expect(original.snapshot()->routes()[0].hs_speed_factor == f(3), "snapshot retains authored factor");
    expect(original.graph() && original.graph()->node(2)->edges.back().cost == f(30), "session builds authoritative graph");
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto session = eawr::campaign::GameSession::create(value).value();
        session.scramble_storage_for_testing();
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 8; ++tick) {
            const auto step = session.step(executor).value();
            hashes.push_back(step.state_hash.get());
            expect(session.galaxy().state_sha256() == session.galaxy().cached_state_hash().get(), "cached graph digest equals full encoding");
            expect(session.state_sha256() == step.state_hash.get(), "campaign graph digest includes the mode timeline");
            expect(step.galaxy->canonical_bytes() == session.galaxy().snapshot()->canonical_bytes(), "published graph snapshot equals session snapshot");
            const auto path = gc::find_planet_path(*session.galaxy().graph(), 1, 3, observations(*session.galaxy().graph())).value();
            expect(path.planets == std::vector<gc::PlanetId>{1, 2, 3} && path.cost == f(40), "worker/storage path parity");
        }
        if (workers == 1) reference = hashes; else expect(hashes == reference, "1/2/4/8 graph session hashes equal");
        const auto identities = eawr::checkpoint::game_identities(value);
        const auto saved = eawr::checkpoint::save_game_checkpoint(session, identities).value();
        eawr::checkpoint::GameLoadOptions options;
        auto restored = eawr::checkpoint::load_game_checkpoint(saved, identities, options, executor);
        expect(static_cast<bool>(restored), "graph checkpoint reconstruction");
        if (restored) expect(restored.value().session.galaxy().setup() == value && restored.value().session.state_sha256() == session.state_sha256(), "checkpoint preserves graph rules and state");
    }
}
void partitioned_parity() {
    gc::SyntheticGalaxyOptions spec; spec.planets = 257; spec.players = 3;
    auto value = gc::synthetic_galaxy(spec);
    value.rules.maximum_fleet_movement_distance = f(30);
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        galactic_test::PhaseProbe probe(pool);
        auto session = gc::GalacticSession::create(value).value();
        session.scramble_storage_for_testing();
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 4; ++tick) {
            const auto step = session.step(probe).value();
            expect(step.work.gathered_objects >= 257 && step.work.sections_reused == (tick == 0 ? 0U : 5U), "graph section cached during partitioned service");
            expect(step.state_hash.get() == session.state_sha256(), "partitioned cached hash equals full state");
            hashes.push_back(step.state_hash.get());
        }
        expect(probe.phases.front().first == "galactic-gather" && probe.phases.front().second > 1, "graph-enabled setup exercises partitioned gather");
        const auto& graph = *session.graph(); const auto access = observations(graph);
        std::vector<gc::PlanetPath> results(16);
        expect(static_cast<bool>(pool.execute_phase("graph-query-contract", results.size(), [&](const auto partition) {
            results[partition] = gc::find_planet_path(graph, value.planets[partition].id, value.planets.back().id, access).value();
        })), "concurrent immutable graph queries");
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto serial = gc::find_planet_path(graph, value.planets[i].id, value.planets.back().id, access).value();
            expect(results[i].planets == serial.planets && results[i].cost == serial.cost && results[i].work == serial.work, "concurrent query bytes and work equal serial");
        }
        if (workers == 1) reference = hashes; else expect(hashes == reference, "partitioned graph 1/2/4/8 hash parity");
    }
}
std::optional<std::string> game_root() {
    return eawr::testing::game_root();
}
void installed_remake(const std::filesystem::path& mod_root) {
    const auto root = game_root();
    expect(root.has_value(), "Remake canary requires EAWR_EAW_GAME_ROOT"); if (!root) return;
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, directory] : std::vector<std::pair<std::string, std::filesystem::path>>{
            {"mod", mod_root / "Data"}, {"expansion", std::filesystem::path(*root) / "corruption/Data"},
            {"base", std::filesystem::path(*root) / "GameData/Data"}}) {
        auto resolved = eawr::vfs::resolve_manifest_mount(id, directory);
        expect(static_cast<bool>(resolved), "Remake layered manifest mount"); if (!resolved) return;
        mounts.push_back(std::move(resolved).value().mount);
    }
    const auto filesystem = eawr::vfs::Vfs::mount(mounts).value();
    const auto catalog = eawr::data::load_catalog(filesystem, eawr::data::Profile::remake).value();
    const auto data = eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog).value();
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    const auto* campaign = data.campaign("GC_Mini_Web_Way_Rebel");
    expect(campaign != nullptr, "installed Remake canary definition"); if (!campaign) return;
    const auto model = eawr::campaign::build_campaign_galaxy(data, types, *campaign, 1, 1);
    expect(static_cast<bool>(model), "installed unmodified Remake canary passes graph admission");
    if (!model) { std::cerr << eawr::core::format_diagnostic(model.error()) << '\n'; return; }
    expect(model.value().planets.size() == campaign->planets.size(), "canary keeps all authored planets");
    expect(model.value().planets.size() == 105 && campaign->trade_routes.size() == 139
        && model.value().routes.size() == 135, "installed canary retains exactly 105 planets and 135 of 139 routes");
    expect(model.value().diagnostics.size() == 4, "installed canary emits exactly four admission warnings");
    for (const auto* name : {"WW_064", "WW_126", "WW_127"}) {
        const auto count = std::count_if(model.value().diagnostics.begin(), model.value().diagnostics.end(),
            [&](const auto& row) {
                return row.code == "campaign_route_external_endpoint" && row.severity == eawr::core::Severity::warning
                    && row.message.find(std::string("skipped route ") + name + ":") != std::string::npos;
            });
        expect(count == 1, std::string("installed canary external endpoint warning: ") + name);
    }
    expect(std::count_if(model.value().diagnostics.begin(), model.value().diagnostics.end(), [](const auto& row) {
        return row.code == "campaign_route_duplicate_pair" && row.severity == eawr::core::Severity::warning
            && row.message.ends_with(": WW_288");
    }) == 1, "installed canary duplicate pair warning: WW_288");
    for (const auto& diagnostic : model.value().diagnostics)
        std::cout << eawr::core::format_diagnostic(diagnostic) << '\n';
    std::cout << "Remake canary: planets=" << model.value().planets.size() << " routes=" << model.value().routes.size()
        << " authored-routes=" << campaign->trade_routes.size() << " warnings=" << model.value().diagnostics.size() << '\n';
}
void installed() {
    const auto root = game_root();
    if (!root) { eawr::testing::skip_installed("SKIPPED: stock galaxy graphs need EAWR_EAW_GAME_ROOT\n", 0, std::cout); return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}}) {
        auto mount = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(mount), "stock manifest mount"); if (!mount) return;
        mounts.push_back(std::move(mount).value().mount);
    }
    const auto filesystem = eawr::vfs::Vfs::mount(mounts).value();
    const auto catalog = eawr::data::load_catalog(filesystem, eawr::data::Profile::foc).value();
    const auto data = eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog).value();
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    std::size_t campaigns{};
    std::size_t background_rows{};
    for (const auto& campaign : data.campaigns) {
        for (const auto& location : campaign.locations) {
            const auto* art = data.galaxy_art_object(location);
            const auto* authored = art ? art : data.planet(location);
            if (!authored || authored->in_background != true) continue;
            ++background_rows;
            std::cout << "stock background " << campaign.name << ": " << location
                << " planetary-behavior=" << authored->has_planetary_behavior() << " art=" << (art != nullptr) << '\n';
            expect(art && !authored->has_planetary_behavior() && !data.planet(location)
                && std::none_of(campaign.planets.begin(), campaign.planets.end(), [&](const auto& name) {
                    return data.galaxy_art_object(name) == art;
                }), "stock In_Background record is non-conquerable art: " + campaign.name + "/" + location);
        }
        if (campaign.planets.empty()) continue;
        const auto model = eawr::campaign::build_campaign_galaxy(data, types, campaign, 1, 1);
        expect(static_cast<bool>(model), "stock campaign galaxy " + campaign.name);
        if (!model) { std::cerr << eawr::core::format_diagnostic(model.error()) << '\n'; continue; }
        ++campaigns;
        expect(model.value().diagnostics.empty(), "stock route admission has no new warnings");
        expect(std::none_of(model.value().names.begin(), model.value().names.end(), [&](const auto& row) {
            return data.galaxy_art_object(row.name) != nullptr;
        }), "stock graph/state names exclude every galaxy art record");
        auto value = setup(); value.planets = model.value().planets; value.routes = model.value().routes;
        value.rules.maximum_fleet_movement_distance = model.value().maximum_movement_distance;
        value.content_identity = model.value().content_identity;
        auto session = gc::GalacticSession::create(value);
        expect(static_cast<bool>(session), "stock loaded objects become session state"); if (!session) continue;
        const auto& graph = *session.value().graph(); const auto n = graph.nodes().size();
        expect(graph.work().pairs_tested == n * (n - 1) / 2, "stock all-pair work count");
        expect(value.routes.size() == campaign.trade_routes.size(), "campaign-scoped routes");
        for (const auto& route : value.routes) {
            const auto* node = graph.node(route.from);
            expect(std::any_of(node->edges.begin(), node->edges.end(), [&](const auto& e) { return e.to == route.to && e.route == route.id; }), "stock named lane wired");
            const auto route_path = gc::find_planet_path(graph, route.from, route.to, observations(graph)).value();
            expect(!route_path.planets.empty(), "stock named route endpoints reachable in " + campaign.name);
        }
        // Campaign membership does not promise global connectivity: isolated/script planets
        // are legal. Independently flood adjacency, then check the weighted search agrees.
        std::set<gc::PlanetId> reachable{graph.nodes().front().id};
        std::vector<gc::PlanetId> pending{graph.nodes().front().id};
        for (std::size_t index = 0; index < pending.size(); ++index)
            for (const auto& edge : graph.node(pending[index])->edges)
                if (reachable.insert(edge.to).second) pending.push_back(edge.to);
        const auto path = gc::find_planet_path(graph, graph.nodes().front().id, graph.nodes().back().id, observations(graph)).value();
        expect(path.planets.empty() == !reachable.contains(graph.nodes().back().id), "stock path agrees with independent reachability in " + campaign.name);
        if (!path.planets.empty()) {
            expect(path.planets.front() == graph.nodes().front().id && path.planets.back() == graph.nodes().back().id, "stock path endpoints");
            Fixed cost;
            for (std::size_t i = 1; i < path.planets.size(); ++i) {
                const auto& edges = graph.node(path.planets[i - 1])->edges;
                const auto edge = std::find_if(edges.begin(), edges.end(), [&](const auto& row) { return row.to == path.planets[i]; });
                expect(edge != edges.end(), "stock path uses only graph connections");
                if (edge != edges.end()) cost = eawr::sim::math::add(cost, edge->cost).value();
            }
            expect(cost == path.cost, "stock returned path cost equals its hop costs");
        }
        std::cout << "stock graph " << campaign.name << ": planets=" << n << " routes=" << value.routes.size()
            << " first-component=" << reachable.size() << " endpoint-reachable=" << !path.planets.empty() << '\n';
    }
    expect(campaigns >= 30, "stock campaign family coverage");
    expect(background_rows >= 30, "stock background classification coverage across campaign families");
}
} // namespace
int case_galactic_galaxy_graph_tests(const int argc, const char* const argv[]) {
    if (argc == 3 && std::string_view(argv[1]) == "--generate") {
        const std::filesystem::path directory(argv[2]); std::filesystem::create_directories(directory);
        const auto value = setup(); auto session = eawr::campaign::GameSession::create(value).value();
        const eawr::sim::InlineExecutor executor;
        std::ofstream hashes(directory / "graph-rules.hashes.csv", std::ios::binary); hashes << "tick,sha256\n";
        for (int tick = 0; tick < 8; ++tick) {
            const auto step = session.step(executor).value(); hashes << step.completed_tick << ',' << step.state_hash.get() << '\n';
        }
        const auto bytes = gc::write_replay(session.record()).value();
        std::ofstream replay(directory / "graph-rules.eawr-gcreplay", std::ios::binary);
        replay.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return replay && hashes ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--remake") installed_remake(argv[2]);
    else if (argc > 1) installed(); else { paths(); gates(); campaign_boundary(); external_endpoints(); persistence(); partitioned_parity(); }
    return galactic_test::failures == 0 ? 0 : 1;
}
