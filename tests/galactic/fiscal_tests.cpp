#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/income.hpp"
#include "eawr/sim/galactic/observer.hpp"

#include <limits>
#include <exception>

namespace {
using namespace galactic_test;
namespace cp = eawr::checkpoint;
namespace campaign = eawr::campaign;
using eawr::sim::math::Fixed;
using eawr::testing::result_code_colon::take;
Fixed f(std::int64_t n) { return take(Fixed::from_integer(n)); }
galactic::GalacticSetup setup() {
    galactic::SyntheticGalaxyOptions options;
    options.players = 3; options.planets = 6; options.fleets_per_player = 1;
    options.members_per_fleet = 1; options.debug_commands = true;
    auto config = galactic::synthetic_galaxy(options);
    config.rules.fiscal_cycle_frames = 3;
    config.player_economy = galactic::PlayerEconomySetup{{{1, f(10), f(100), 2, 4}, {2, f(20), f(100), 1, 5}, {3, f(30), f(100), 0, 0}}};
    config.fiscal = galactic::FiscalSetup{f(50), {{1, true, f(1)}, {2, true, take(Fixed::from_decimal("1.2"))}, {3, false, f(1)}}, {}};
    for (const auto& planet : config.planets) config.fiscal->planets.push_back({planet.id, f(10)});
    return config;
}
Fixed balance(const galactic::GalacticSession& game, std::size_t i) {
    return game.snapshot()->player_economy()->players[i].credits;
}
galactic::PlanetIncomeRules rules(const galactic::GalacticSetup& config) {
    galactic::PlanetIncomeRules out;
    out.maximum_alignment_bonus = out.maximum_alignment_penalty = take(Fixed::from_decimal("0.5"));
    out.political_curve = take(galactic::parse_income_curve("0,0,1,1,5,1"));
    for (const auto& player : config.players) out.factions.push_back({player.player_id});
    for (const auto& planet : config.planets) out.planets.push_back({planet.id});
    for (const auto& route : config.routes) out.routes.push_back({route.id});
    return out;
}
void retail_c01() {
    // GHUD-R01 / WGEP-06..09: source-shaped numeric fixture, no retail assets.
    // Effective Planet_Credit_Value: Shola35, Coruscant160, Kamino75,
    // Honoghr85, VergessoAsteroids75. All five author neutral/zero strength.
    galactic::GalacticSetup config;
    config.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    config.planets = {{1, {}, 1}, {2, {f(10), {}, {}}, 1}, {3, {f(20), {}, {}}, 2},
        {4, {f(30), {}, {}}, 2}, {5, {f(40), {}, {}}, 1}};
    config.routes = {{6, 1, 2}, {7, 2, 3}, {8, 4, 3}, {9, 1, 5}};
    config.rules.fiscal_cycle_frames = 3;
    config.player_economy = galactic::PlayerEconomySetup{{{1, f(12000), f(120000), 2, 5},
        {2, f(8000), f(80000), 2, 5}, {3, {}, {}, 0, 0}}};
    config.fiscal = galactic::FiscalSetup{f(40000), {{1, true, f(1)}, {2, true, f(1)}, {3, false, f(1)}},
        {{1, f(35)}, {2, f(160)}, {3, f(75)}, {4, f(85)}, {5, f(75)}}};
    config.fiscal->contributions = rules(config);
    auto& inputs = *config.fiscal->contributions;
    inputs.factions[0].alignment = galactic::IncomeAlignment::dark;
    // Four authored links factor0.5; Coruscant->Kamino is hostile and adds nothing.
    for (auto& route : inputs.routes) route.factor = take(Fixed::from_decimal("0.5"));
    config.observer.emplace(); auto& observer = *config.observer;
    observer.human_observers = {1}; observer.solo_reference_human = 1;
    for (const auto& player : config.players) observer.players.push_back({player.player_id});
    for (const auto& planet : config.planets) {
        galactic::ObserverPlanet row; row.planet = planet.id;
        row.allegiance = row.target_owner = row.current_owner = row.current_controller = planet.owner;
        observer.planets.push_back(row);
        for (const auto& player : config.players) {
            galactic::ObserverRecord record{player.player_id, planet.id, true};
            if (player.player_id == planet.owner) record.mask = galactic::owner_intelligence;
            observer.records.push_back(record);
        }
    }
    std::sort(observer.records.begin(), observer.records.end(), [](const auto& a, const auto& b) {
        return std::pair{a.player, a.planet} < std::pair{b.player, b.planet};
    });
    const auto native_trade = take(Fixed::from_decimal("152.5"));
    const auto native_total = take(Fixed::from_decimal("422.5"));
    const galactic::GalacticReplay replay{config, 22, {}};
    std::optional<std::string> reference;
    for (auto count : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter workers(count);
        auto game = take(campaign::GameSession::from_replay(replay));
        const auto* forecast = game.galaxy().snapshot()->economy_forecast();
        expect(forecast && forecast->base == f(270) && forecast->trade_routes == std::optional(native_trade)
            && forecast->gross_total == native_total && forecast->alignment == std::optional(f(0)),
            "C01 native base270 + trade152.5 =422.5 with political writer zero");
        expect(forecast->planets[1].actual == std::optional(f(240)), "C01 Coruscant observed income240");
        for (int i = 0; i < 2; ++i) {
            const auto tick = take(game.step(workers));
            expect(tick.galaxy_work.income_forecast_rebuilds == 0, "unchanged C01 forecast skips contribution rebuild");
        }
        const auto identities = cp::game_identities(config);
        const auto saved = take(cp::save_game_checkpoint(game, identities));
        auto restored = take(cp::load_game_checkpoint(saved, identities, {}, workers));
        while (game.completed_tick() < 22) {
            const auto tick = take(game.step(workers));
            expect(take(restored.session.step(workers)).state_sha256 == tick.state_sha256, "C01 fractional payment checkpoint continuation");
            const auto credits = game.galaxy().snapshot()->player_economy()->players[0].credits;
            if (game.completed_tick() == 4) expect(credits == take(Fixed::from_decimal("12422.5")), "C01 Day2 native balance");
            if (game.completed_tick() == 7) expect(credits == f(12845), "C01 Day3 native balance");
        }
        expect(game.galaxy().snapshot()->player_economy()->players[0].credits == take(Fixed::from_decimal("14957.5")), "C01 Day8 native balance");
        if (!reference) reference = game.state_sha256();
        expect(game.state_sha256() == *reference, "C01 1/2/4/8 equality");
    }
}
void contribution_math() {
    const auto half = take(Fixed::from_decimal("0.5"));
    auto value = take(galactic::planet_income_contribution(f(100), f(2), f(20), f(1), {}, half, half, half, half));
    expect(value.base == f(200) && value.bonus == f(20) && value.alignment == f(50)
        && value.political == f(-50) && value.trade == f(50) && value.total == f(270),
        "WGEP-06 keeps additive sources outside black-market multiplier");
    value = take(galactic::planet_income_contribution(f(100), f(1), {}, {}, half, half, half, {}, {}));
    expect(value.alignment == f(-25) && value.total == f(75), "WGEP-07 opposing alignment penalty");
    const auto curve = take(galactic::parse_income_curve("5,1,0,0,1,1"));
    expect(take(galactic::evaluate_income_curve(curve, f(1))) == f(1), "curve at point");
    expect(take(galactic::evaluate_income_curve(curve, half)) == half, "piecewise interpolation");
    expect(take(galactic::evaluate_income_curve(curve, f(3))) == f(1), "flat upper segment");
    expect(take(galactic::evaluate_income_curve(curve, f(-1))) == f(0)
        && take(galactic::evaluate_income_curve(curve, f(6))) == f(0), "no extrapolation");
    expect(take(galactic::evaluate_income_curve(std::span(curve).first(1), {})) == f(0), "insufficient points zero");
    expect(!galactic::parse_income_curve("0,0,0,1") && !galactic::parse_income_curve("0,0,1"), "duplicate and incomplete pairs reject");
    expect(!galactic::planet_income_contribution(Fixed::from_raw(std::numeric_limits<std::int64_t>::max()), f(2), {}, {}, {}, {}, {}, {}, {}),
        "overflow is typed failure");
}
void unclamped_alignment() {
    for (const auto strength : {f(-1), f(2), f(4)}) {
        auto config = setup();
        config.fiscal->contributions = rules(config);
        auto& inputs = *config.fiscal->contributions;
        inputs.factions[0].alignment = galactic::IncomeAlignment::light;
        inputs.planets[0].alignment = galactic::IncomeAlignment::light;
        inputs.planets[0].strength = strength;
        expect(bool(galactic::validate_setup(config)), "CGS-16 signed unclamped strength admitted");
        auto game = take(galactic::GalacticSession::create(config));
        eawr::platform::ThreadWorkerAdapter pool(1);
        for (int i = 0; i < 4; ++i) (void)take(game.step(pool));
        const auto effect = strength.raw() > 0 ? take(eawr::sim::math::multiply(f(5), strength)) : f(0);
        expect(balance(game, 0) == take(eawr::sim::math::add(f(30), effect)),
            "CGS-16 income keeps strength magnitude and ignores nonpositive products");
    }
}
void trade_and_alignment() {
    auto config = setup();
    config.fiscal->credit_cap_per_planet = f(10000);
    config.fiscal->contributions = rules(config);
    auto& inputs = *config.fiscal->contributions;
    inputs.factions[0].alignment = galactic::IncomeAlignment::light;
    inputs.planets[0].alignment = galactic::IncomeAlignment::light;
    inputs.planets[0].strength = f(1);
    inputs.planets[3].alignment = galactic::IncomeAlignment::dark;
    inputs.planets[3].strength = take(Fixed::from_decimal("0.5"));
    // The synthetic graph has an allied 1->4 edge and hostile 1->2 edge.
    for (std::size_t i = 0; i < config.routes.size(); ++i) inputs.routes[i].factor =
        config.routes[i].from == 1 && config.routes[i].to == 4 ? take(Fixed::from_decimal("0.5")) : f(100);
    std::optional<std::string> reference;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(galactic::GalacticSession::create(config));
        for (int i = 0; i < 4; ++i) (void)take(game.step(pool));
        // basic20 + alignment(5-2.5) + allied trade10; hostile links ignored.
        expect(balance(game, 0) == take(Fixed::from_decimal("42.5")), "live owner alignment and hostile trade exclusion");
        if (!reference) reference = game.state_sha256();
        expect(game.state_sha256() == *reference, "new contributions worker parity");
    }
    auto allied_config = config;
    allied_config.fleets.clear();
    allied_config.control.emplace();
    auto& control = *allied_config.control;
    control.change_seconds = f(7); control.max_ground_forces = 10;
    control.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (auto a : {1U, 2U, 3U}) for (auto b : {1U, 2U, 3U}) control.relations.push_back({a, b, a == b || (a != 3 && b != 3)});
    for (const auto& planet : allied_config.planets) {
        galactic::ControlPlanet row; row.planet = planet.id; row.type = 100 + planet.id;
        row.ground_base = {planet.owner, 1}; control.planets.push_back(row);
    }
    for (const auto& route : allied_config.routes) control.routes.push_back({route.id});
    for (auto& route : allied_config.fiscal->contributions->routes) route.factor = {};
    allied_config.fiscal->contributions->routes[0].factor = take(Fixed::from_decimal("0.5")); // 1->2 allied different owners
    eawr::sim::InlineExecutor serial;
    auto allied_game = take(galactic::GalacticSession::create(allied_config));
    for (int i = 0; i < 4; ++i) (void)take(allied_game.step(serial));
    expect(balance(allied_game, 0) == take(Fixed::from_decimal("37.5")), "cross-owner allied endpoint adds trade");
    for (auto factor : {f(0), f(-1)}) {
        for (auto& route : inputs.routes) route.factor = factor;
        eawr::sim::InlineExecutor executor;
        auto game = take(galactic::GalacticSession::create(config));
        for (int i = 0; i < 4; ++i) (void)take(game.step(executor));
        expect(balance(game, 0) == take(Fixed::from_decimal("32.5")), "nonpositive trade factors ignored");
    }
}
void malformed_static_tail() {
    auto config = setup();
    config.fiscal->contributions = rules(config);
    const auto bytes = take(galactic::write_replay({config, 4, {}}));
    const auto read32 = [&](std::size_t at) {
        return std::uint32_t(bytes[at]) | (std::uint32_t(bytes[at+1]) << 8)
            | (std::uint32_t(bytes[at+2]) << 16) | (std::uint32_t(bytes[at+3]) << 24);
    };
    const auto put32 = [](auto& output, std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) output[at+i] = static_cast<std::uint8_t>(value >> (8*i));
    };
    std::size_t extension = 100;
    while (bytes[extension] != 6 || bytes[extension+1] != 0) extension += 6 + read32(extension+2);
    const auto payload = extension + 6, end = payload + read32(extension+2);
    auto cursor = payload + 16 + config.fiscal->players.size()*13 + config.fiscal->planets.size()*16;
    const auto tail = cursor;
    std::vector<std::size_t> boundaries;
    const auto field = [&](std::size_t width) { cursor += width; boundaries.push_back(cursor); };
    field(1); field(8); field(8); field(4);
    for (const auto& point : config.fiscal->contributions->political_curve) { (void)point; field(8); field(8); }
    const auto faction_count = cursor;
    field(4);
    for (const auto& player : config.players) { (void)player; field(4); field(1); }
    field(4);
    for (const auto& planet : config.planets) { (void)planet; field(8); field(1); field(8); }
    field(4);
    for (const auto& route : config.routes) { (void)route; field(8); field(8); }
    expect(cursor == end, "tail boundary inventory covers every encoded field");
    const auto malformed = [](const auto& candidate, std::string_view message) {
        const auto parsed = galactic::parse_replay(candidate);
        expect(!parsed && parsed.error().code == galactic::diagnostic_codes::replay_malformed, message);
    };
    for (auto boundary : boundaries) {
        if (boundary == end) continue;
        auto shortened = bytes;
        shortened.erase(shortened.begin()+static_cast<std::ptrdiff_t>(boundary), shortened.begin()+static_cast<std::ptrdiff_t>(end));
        put32(shortened, extension+2, static_cast<std::uint32_t>(boundary-payload));
        malformed(shortened, "truncation at every static-tail field boundary is malformed");
    }
    auto bad = bytes; bad[tail] = 255;
    malformed(bad, "unknown static-tail version is malformed");
    bad = bytes; put32(bad, faction_count, static_cast<std::uint32_t>(config.players.size()-1));
    malformed(bad, "fiscal static table count mismatch is malformed");
    bad = bytes; bad.insert(bad.begin()+static_cast<std::ptrdiff_t>(end), 0);
    put32(bad, extension+2, read32(extension+2)+1);
    malformed(bad, "trailing byte after valid static tail is malformed");
    auto invalid = config; invalid.fiscal->contributions->maximum_alignment_bonus = f(2);
    expect(!galactic::validate_setup(invalid), "alignment maximum bonus above one rejected");
    invalid = config; invalid.fiscal->contributions->maximum_alignment_penalty = f(2);
    expect(!galactic::validate_setup(invalid), "alignment maximum penalty above one rejected");
}
void preview(bool trade = false) {
    auto config = setup();
    config.observer.emplace();
    auto& observer = *config.observer;
    observer.human_observers = {1}; observer.solo_reference_human = 1;
    for (const auto& player : config.players) observer.players.push_back({player.player_id});
    for (const auto& planet : config.planets) {
        galactic::ObserverPlanet row;
        row.planet = planet.id;
        row.allegiance = row.target_owner = row.current_owner = row.current_controller = planet.owner;
        observer.planets.push_back(row);
    }
    for (const auto& fleet : config.fleets) for (const auto& member : fleet.members)
        observer.types.push_back({member.type});
    std::sort(observer.types.begin(), observer.types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    observer.types.erase(std::unique(observer.types.begin(), observer.types.end()), observer.types.end());
    for (const auto& player : config.players) for (const auto& planet : config.planets)
        observer.records.push_back({player.player_id, planet.id, true});
    // Synthetic forecast contract; retail C01 is covered separately.
    for (auto& planet : config.fiscal->planets) planet.credits = f(135);
    config.fiscal->credit_cap_per_planet = f(10000);
    const auto expected_total = f(trade ? 405 : 270);
    if (trade) {
        config.fiscal->contributions = rules(config);
        for (std::size_t i = 0; i < config.routes.size(); ++i)
            if (config.routes[i].from == 1 && config.routes[i].to == 4)
                config.fiscal->contributions->routes[i].factor = take(Fixed::from_decimal("0.5"));
    }
    // The ledger cap is already published at startup; no ownership/control
    // service runs in this fixture to refresh it from credit_cap_per_planet.
    for (auto& account : config.player_economy->players) account.credit_cap = f(20000);
    std::optional<galactic::EconomyForecast> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(galactic::GalacticSession::create(config));
        const auto before = balance(game, 0);
        const auto* forecast = game.snapshot()->economy_forecast();
        expect(forecast && forecast->player == 1 && forecast->base == f(270)
            && forecast->bonus == f(0) && forecast->gross_total == expected_total, "preview equals current C01 base-only collection");
        expect(!forecast->political && !forecast->corruption, "missing source-ledger providers remain unknown");
        expect(trade ? forecast->trade_routes == std::optional(f(135)) && forecast->alignment == std::optional(f(0))
            : !forecast->trade_routes && !forecast->alignment, "known trade and alignment match the payment provider");
        for (const auto& row : forecast->planets) {
            const auto owner = std::find_if(config.planets.begin(), config.planets.end(), [&](const auto& p) { return p.id == row.planet; });
            if (owner->owner != 1) expect(!row.actual, "foreign actual income stays masked");
            expect(row.basic == f(135), "discovered basic income is a separate visible fallback");
        }
        const auto copy = *forecast;
        auto reversed_config = config;
        std::reverse(reversed_config.observer->planets.begin(), reversed_config.observer->planets.end());
        expect(!galactic::GalacticSession::create(reversed_config), "session rejects unordered observer planet facts");
        const auto input = game.snapshot();
        auto reversed_observations = std::make_shared<std::vector<galactic::ObservedPlanet>>(
            input->observations().begin(), input->observations().end());
        std::reverse(reversed_observations->begin(), reversed_observations->end());
        galactic::GalacticSnapshot reversed(input->completed_tick(), input->clock(),
            {input->players().begin(), input->players().end()},
            {input->planets().begin(), input->planets().end()},
            {input->routes().begin(), input->routes().end()},
            {input->fleets().begin(), input->fleets().end()}, {}, input->maximum_movement_distance(),
            input->fleet_control(), {}, input->seat_control(), {input->seats().begin(), input->seats().end()},
            input->player_economy(), input->production(), std::nullopt, {},
            {input->population().begin(), input->population().end()}, *input->observer(),
            std::nullopt, std::nullopt, {}, reversed_observations, copy);
        expect(*reversed.economy_forecast() == copy,
            "reversed observation projection preserves sorted forecast lookup rows");
        expect(forecast->adjusted_payment == std::optional(expected_total), "uncapped payment uses the collection adjustment");
        for (int i = 0; i < 4; ++i) (void)take(game.step(pool));
        expect(balance(game, 0) == take(eawr::sim::math::add(before, copy.gross_total)), "forecast does not collect and matches next payment");
        forecast = game.snapshot()->economy_forecast();
        if (!reference) reference = *forecast;
        expect(*forecast == *reference, "preview 1/2/4/8 equality");
        const auto snapshot = game.snapshot();
        // Keep every canonical section (movement, income, victory, corruption, planet access, census)
        // and omit only the forecast.
        const auto* movement = snapshot->movement();
        const auto* access = snapshot->planet_access();
        const auto* control = snapshot->control();
        const auto* income = snapshot->income();
        const auto* corruption = snapshot->economy_corruption();
        galactic::GalacticSnapshot without_preview(snapshot->completed_tick(), snapshot->clock(),
            {snapshot->players().begin(), snapshot->players().end()},
            {snapshot->planets().begin(), snapshot->planets().end()},
            {snapshot->routes().begin(), snapshot->routes().end()},
            {snapshot->fleets().begin(), snapshot->fleets().end()},
            {snapshot->events().begin(), snapshot->events().end()}, snapshot->maximum_movement_distance(),
            snapshot->fleet_control(),
            movement ? std::make_shared<const galactic::MovementSetup>(*movement) : nullptr,
            snapshot->seat_control(), {snapshot->seats().begin(), snapshot->seats().end()},
            snapshot->player_economy(), snapshot->production(),
            control ? std::optional(*control) : std::nullopt,
            {snapshot->control_effects().begin(), snapshot->control_effects().end()},
            {snapshot->population().begin(), snapshot->population().end()}, *snapshot->observer(),
            income ? std::optional(*income) : std::nullopt, snapshot->victory(), {}, {}, std::nullopt, {}, std::nullopt,
            std::nullopt, corruption ? std::optional(*corruption) : std::nullopt,
            access ? std::make_shared<const galactic::PlanetAccessState>(*access) : nullptr, 0, {},
            std::make_shared<const std::vector<galactic::CensusObject>>(
                snapshot->actor_records().begin(), snapshot->actor_records().end()),
            {}, {}, {}, {}, {}, {}, {}, {},
            // The History publication is canonical (snapshot format 7), so the rebuild must carry it too.
            galactic::SharedSnapshotSections{.history = snapshot->history() ? std::make_shared<const galactic::HistorySnapshot>(*snapshot->history()) : nullptr},
            snapshot->view_player());
        expect(without_preview.canonical_bytes() == snapshot->canonical_bytes(), "preview excluded from canonical bytes");
        expect(bool(game.submit(owner(game.completed_tick(), {1}, 1, config.planets[0].id, 2))), "cached forecast owner transfer admitted");
        const auto changed = take(game.step(pool));
        expect(changed.work.income_forecast_rebuilds == 1, "owner change rebuilds contribution cache once");
        expect(game.snapshot()->economy_forecast()->gross_total == f(135), "owner change invalidates both base and allied trade");
        expect(take(game.step(pool)).work.income_forecast_rebuilds == 0, "unchanged next frame reuses refreshed cache");
    }
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto capped_config = config;
        capped_config.player_economy->players[0].credits = f(19990);
        auto capped = take(galactic::GalacticSession::create(capped_config));
        const auto before = balance(capped, 0);
        const auto predicted = *capped.snapshot()->economy_forecast();
        expect(predicted.gross_total == expected_total, "gross income independent of cap");
        expect(predicted.adjusted_payment == std::optional(f(10)), "near-cap payment uses adjusted_credits");
        for (int i = 0; i < 4; ++i) (void)take(capped.step(pool));
        expect(take(eawr::sim::math::subtract(balance(capped, 0), before)) == *predicted.adjusted_payment,
            "capped preview equals actual next credit delta on 1/2/4/8");
        auto ai_config = config; ai_config.observer->human_observers = {2}; ai_config.observer->solo_reference_human = 2;
        auto adjusted = take(galactic::GalacticSession::create(ai_config));
        const auto ai_before = balance(adjusted, 1);
        const auto ai_preview = *adjusted.snapshot()->economy_forecast();
        const auto ai_income = take(eawr::sim::math::multiply(f(270), config.fiscal->players[1].positive_credit_multiplier));
        expect(ai_preview.adjusted_payment == std::optional(ai_income), "uncapped forecast applies positive multiplier");
        for (int i = 0; i < 4; ++i) (void)take(adjusted.step(pool));
        expect(take(eawr::sim::math::subtract(balance(adjusted, 1), ai_before)) == *ai_preview.adjusted_payment,
            "uncapped adjusted preview equals actual next credit delta on 1/2/4/8");
    }
    config.observer.reset();
    auto missing = take(galactic::GalacticSession::create(config));
    expect(!missing.snapshot()->economy_forecast(), "no observer means no local forecast");
}
void runtime_saturation() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto config = setup();
        config.player_economy->players[0].credits = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
        auto game = take(galactic::GalacticSession::create(config));
        eawr::platform::ThreadWorkerAdapter pool(workers);
        for (std::size_t i = 0; i < 10; ++i) {
            auto tick = game.step(pool);
            expect(tick.has_value(), "C overflowing fiscal account saturates and GC keeps stepping");
            if (!tick) break;
            if (workers == 1) reference.push_back(tick.value().state_hash.get());
            else expect(tick.value().state_hash.get() == reference[i], "fiscal saturation matches on 1/2/4/8 workers");
        }
        expect(balance(game, 0) == f(100), "overflowing positive fiscal addition uses the account cap");
        const auto notes = game.load_notes();
        expect(std::count_if(notes.begin(), notes.end(), [](const auto& note) {
            return note.code == "EAWR-LOAD-1111" && note.object == "1"
                && note.phase == eawr::core::LoadPhase::runtime
                && note.effective_value == std::to_string(f(100).raw()) && !note.diagnostic_code.empty();
        }) == 1, "C fiscal note once per player/code with effective cap and original diagnostic");
    }
}
void accounts() {
    expect(take(galactic::adjusted_credits(f(10), f(10), take(Fixed::from_decimal("0.5")), f(100))) == f(15), "AI positive additions use difficulty");
    expect(take(galactic::adjusted_credits(f(10), f(-8), f(2), f(100))) == f(2), "debit is unmultiplied");
    expect(take(galactic::adjusted_credits(f(10), f(-20), f(2), f(100))) == f(0), "debit clamps at zero");
    expect(take(galactic::adjusted_credits(f(95), f(10), f(2), f(100))) == f(100), "addition clamps at cap");
    expect(!galactic::adjusted_credits(Fixed::from_raw(std::numeric_limits<std::int64_t>::max()), f(1), f(1), f(100)), "addition overflow diagnosed");
}
void signed_planet_income() {
    auto config = setup();
    config.fiscal->players[0].positive_credit_multiplier = f(2);
    config.fiscal->players[1].positive_credit_multiplier = f(2);
    std::vector<unsigned> seen(config.players.size());
    for (std::size_t i = 0; i < config.planets.size(); ++i) {
        const auto owner = config.planets[i].owner;
        const bool first = seen[owner - 1]++ == 0;
        config.fiscal->planets[i].credits = owner == 1 ? f(first ? 2 : -10) : f(first ? 3 : -15);
    }
    // CGS-19: retain each signed contribution before the owner reduction, then
    // debit the aggregate without the positive-income multiplier.
    expect(take(galactic::planet_income_contribution(f(-10), f(1), {}, {}, {}, {}, {}, {}, {})).total == f(-10),
        "negative planet income is not floored before fiscal aggregation");
    const galactic::GalacticReplay replay{config, 7, {}};
    expect(take(galactic::parse_replay(take(galactic::write_replay(replay)))) == replay,
        "signed base income replay round trip");
    std::optional<std::string> reference;
    for (const auto count : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(count);
        auto game = take(galactic::GalacticSession::create(config));
        std::vector<galactic::GalacticSession::CreditNotification> notices;
        game.set_credit_notification_handler([&](const auto& notice) {
            notices.push_back(notice);
            return eawr::core::Result<void>::success();
        });
        for (int i = 0; i < 4; ++i) (void)take(game.step(pool));
        expect(balance(game, 0) == f(2) && balance(game, 1) == f(8), "signed fiscal total debits human and AI equally");
        expect(notices.size() == 2 && notices[0].amount == f(-8) && notices[0].credits == f(2)
            && notices[1].amount == f(-12) && notices[1].credits == f(8),
            "fiscal notification retains signed aggregate after account debit");
        for (int i = 0; i < 3; ++i) (void)take(game.step(pool));
        expect(balance(game, 0) == f(0) && balance(game, 1) == f(0) && balance(game, 2) == f(30),
            "negative fiscal payment floors eligible balances at zero and leaves ineligible account unchanged");
        if (!reference) reference = game.state_sha256();
        expect(game.state_sha256() == *reference, "signed fiscal payment worker parity");
    }
    auto bad = config;
    for (auto& planet : bad.fiscal->planets) planet.credits = Fixed::from_raw(std::numeric_limits<std::int64_t>::min());
    expect(!galactic::validate_setup(bad), "signed base income underflow still rejects");
}
void boundaries() {
    const auto config = setup(); eawr::sim::InlineExecutor serial;
    auto game = take(galactic::GalacticSession::create(config));
    expect(game.clock().fiscal_cycle == 1 && game.clock().fiscal_end == 3, "initial fiscal window cycle one");
    for (int i = 0; i < 3; ++i) expect(take(game.step(serial)).work.fiscal_cycles == 0, "pre-deadline service does not pay");
    const auto tick = take(game.step(serial));
    expect(tick.work.fiscal_cycles == 1 && game.clock().fiscal_cycle == 2, "one payment on deadline before advance");
    expect(balance(game, 0) == f(30) && balance(game, 1) == take(eawr::sim::math::add(f(20), take(eawr::sim::math::multiply(f(20), take(Fixed::from_decimal("1.2")))))), "aggregate planets once and apply AI factor once");
    expect(balance(game, 2) == f(30), "ineligible faction receives no income");
    expect(game.setup() == config && game.record().setup == config, "ledger never changes original replay setup");
    expect(game.state_sha256() == tick.state_sha256, "dirty player digest matches full encoding");
    expect(bool(game.submit({{4, {1}, 1}, galactic::PauseGalaxy{}})), "pause admitted");
    for (int i = 0; i < 6; ++i) (void)take(game.step(serial));
    expect(game.clock().galactic_frame == 4 && balance(game, 0) == f(30), "pause freezes fiscal service");
    expect(bool(game.submit({{10, {1}, 2}, galactic::ResumeGalaxy{}})), "resume admitted");
    for (int i = 0; i < 3; ++i) (void)take(game.step(serial));
    expect(game.clock().fiscal_cycle == 3 && balance(game, 0) == f(50), "resume pays next window once");
    auto owned = setup(); owned.player_economy->players[0].credits = f(90);
    auto transfer = take(galactic::GalacticSession::create(owned));
    expect(bool(transfer.submit(owner(0, {1}, 1, owned.planets[0].id, 2))), "ownership change admitted");
    (void)take(transfer.step(serial));
    expect(balance(transfer, 0) == f(50), "ownership refresh clamps all caps immediately");
    for (std::size_t i = 1; i < owned.planets.size(); ++i) if (owned.planets[i].owner == 1)
        expect(bool(transfer.submit(owner(1, {1}, i + 1, owned.planets[i].id, 2))), "last ownership change admitted");
    (void)take(transfer.step(serial));
    expect(transfer.snapshot()->player_economy()->players[0].credit_cap == f(50), "zero planets retains one-planet cap");
}
void ownership_at_deadline() {
    // GFSC-02 / GFD-02/04/05: the same-frame owner commit and cap clamp precede payment.
    auto config = setup(); config.player_economy->players[0].credits = f(90);
    eawr::sim::InlineExecutor serial;
    auto game = take(galactic::GalacticSession::create(config));
    for (int i = 0; i < 3; ++i) (void)take(game.step(serial));
    expect(bool(game.submit(owner(3, {1}, 1, config.planets[0].id, 2))), "deadline ownership admitted");
    expect(take(game.step(serial)).work.fiscal_cycles == 1, "deadline frame pays once");
    expect(balance(game, 0) == f(50), "losing cap clamps before fiscal addition");
    const auto income = take(eawr::sim::math::multiply(f(30), config.fiscal->players[1].positive_credit_multiplier));
    expect(balance(game, 1) == take(eawr::sim::math::add(f(20), income)), "new owner receives transferred planet income immediately");
}
void combined_extensions() {
    auto config = setup();
    config.rules.maximum_fleet_movement_distance = f(1000);
    config.fleet_control.emplace();
    for (const auto& fleet : config.fleets) for (const auto& member : fleet.members)
        config.fleet_control->types.push_back({member.type, false, true, false});
    auto& types = config.fleet_control->types;
    std::sort(types.begin(), types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    types.erase(std::unique(types.begin(), types.end()), types.end());
    config.movement.emplace();
    auto& movement = *config.movement;
    movement.mode_modifier = f(1); // explicit synthetic input
    for (const auto& type : types)
        movement.types.push_back({type.type, f(1), f(1), 0, true, false, false, false});
    for (const auto& player : config.players) {
        movement.players.push_back({player.player_id, f(1), f(1), false});
        for (const auto& planet : config.planets)
            movement.access.push_back({player.player_id, planet.id, true, false});
    }
    for (const auto& planet : config.planets)
        movement.planets.push_back({planet.id, f(1), false});
    config.seat_control.emplace();
    for (const auto& seat : config.seats) if (seat.kind == galactic::SeatKind::human)
        config.seat_control->teams.push_back({seat.human_side_id, seat.seat_id, false});
    for (const auto& player : config.players) {
        const auto human = std::any_of(config.seats.begin(), config.seats.end(), [&](const auto& seat) {
            return seat.kind == galactic::SeatKind::human && seat.player == player.player_id;
        });
        config.seat_control->controllers.push_back({player.player_id,
            human ? galactic::FactionControl::human : galactic::FactionControl::ai,
            galactic::FactionControl::ai, {}, {}});
    }
    const auto moving_fleet = config.fleets.front().id;
    const galactic::GalacticReplay replay{config, 8, {
        {{0, config.seats.front().seat_id, 1},
            galactic::MoveFleet{moving_fleet, {config.planets[0].id, config.planets[1].id}, {}}}}};
    expect(take(galactic::parse_replay(take(galactic::write_replay(replay)))) == replay,
        "all six graph/fleet/economy/movement/seat/fiscal extensions coexist");
    eawr::sim::InlineExecutor serial;
    auto reference = take(campaign::GameSession::from_replay(replay));
    std::vector<std::string> hashes;
    for (int i = 0; i < 8; ++i) hashes.push_back(take(reference.step(serial)).state_hash.get());
    const auto identities = cp::game_identities(config);
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(campaign::GameSession::from_replay(replay));
        for (int i = 0; i < 4; ++i)
            expect(take(game.step(pool)).state_hash.get() == hashes[i], "combined services worker parity");
        expect(game.galaxy().snapshot()->fleets().front().motion.transit.has_value(),
            "checkpoint retains traveling fleet at fiscal boundary");
        expect(game.galaxy().snapshot()->player_economy()->players[0].credits == f(30),
            "movement and fiscal payment run in the same configured session");
        const auto saved = take(cp::save_game_checkpoint(game, identities));
        auto restored = take(cp::load_game_checkpoint(saved, identities, {}, pool));
        expect(take(cp::save_game_checkpoint(restored.session, identities)) == saved,
            "movement, seat-control and paid fiscal ledger coexist in checkpoint");
        while (restored.session.completed_tick() < 8) {
            const auto index = restored.session.completed_tick();
            restored.session.scramble_storage_for_testing();
            expect(take(restored.session.step(pool)).state_hash.get() == hashes[index],
                "combined checkpoint continuation preserves 1/2/4/8 parity");
        }
    }
}

void replay_and_checkpoint() {
    auto config = setup();
    // Enough objects to exercise the full partitioned gather plus fiscal phase.
    for (std::uint64_t i = 1000; i < 1400; ++i) {
        config.planets.push_back({i, {}, static_cast<galactic::PlayerId>(i % 3 + 1)});
        config.fiscal->planets.push_back({i, f(1)});
    }
    config.fiscal->contributions = rules(config);
    config.fiscal->contributions->routes[1].factor = take(Fixed::from_decimal("0.5"));
    config.fiscal->contributions->factions[0].alignment = galactic::IncomeAlignment::light;
    config.fiscal->contributions->planets[0].alignment = galactic::IncomeAlignment::light;
    config.fiscal->contributions->planets[0].strength = f(1);
    const auto replay = galactic::GalacticReplay{config, 12, {}};
    const auto bytes = take(galactic::write_replay(replay));
    expect(take(galactic::parse_replay(bytes)) == replay, "fiscal wire round trip");
    // Locate the version byte in the tag-6 payload, then garble it.
    auto garbled = bytes;
    std::size_t offset = 100; // fixed header plus extension count
    while (offset + 6 <= garbled.size()) {
        const auto tag = std::uint16_t(garbled[offset]) | (std::uint16_t(garbled[offset+1]) << 8);
        const auto length = std::uint32_t(garbled[offset+2]) | (std::uint32_t(garbled[offset+3]) << 8)
            | (std::uint32_t(garbled[offset+4]) << 16) | (std::uint32_t(garbled[offset+5]) << 24);
        if (tag == 6) {
            const auto tail = offset + 6 + 16 + config.fiscal->players.size() * 13 + config.fiscal->planets.size() * 16;
            garbled[tail] = 255;
            const auto rejected = galactic::parse_replay(garbled);
            expect(!rejected && rejected.error().code == galactic::diagnostic_codes::replay_malformed,
                "unsupported fiscal tail version has typed refusal");
            break;
        }
        offset += 6 + length;
    }
    for (std::size_t n = 0; n < bytes.size(); n += 31)
        expect(!galactic::parse_replay(std::span(bytes).first(n)), "truncated fiscal replay rejected");
    auto bad = config; bad.fiscal->players[0].player = 2; expect(!galactic::validate_setup(bad), "mismatched fiscal players rejected");
    bad = config; bad.fiscal->planets.pop_back(); expect(!galactic::validate_setup(bad), "incomplete planet table rejected");
    bad = config; bad.player_economy.reset(); expect(!galactic::validate_setup(bad), "missing ledger rejected");
    bad = config; bad.fiscal->planets[0].credits = f(-1); expect(bool(galactic::validate_setup(bad)), "signed base income admitted (CGS-19)");
    bad = config; bad.fiscal->credit_cap_per_planet = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    expect(!galactic::validate_setup(bad), "cap multiplication overflow rejected");
    eawr::sim::InlineExecutor serial;
    auto reference = take(campaign::GameSession::from_replay(replay)); std::vector<std::string> hashes;
    for (int i = 0; i < 12; ++i) hashes.push_back(take(reference.step(serial)).state_hash.get());
    const auto identities = cp::game_identities(config);
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        for (const std::uint64_t barrier : {2U, 3U, 4U, 7U}) {
            auto game = take(campaign::GameSession::from_replay(replay));
            while (game.completed_tick() < barrier) (void)take(game.step(pool));
            const auto saved = take(cp::save_game_checkpoint(game, identities));
            auto restored = take(cp::load_game_checkpoint(saved, identities, {}, pool));
            expect(restored.session.state_sha256() == game.state_sha256(), "save restores live ledger at boundary");
            expect(take(cp::save_game_checkpoint(restored.session, identities)) == saved, "canonical fiscal checkpoint");
            while (restored.session.completed_tick() < 12) {
                restored.session.scramble_storage_for_testing();
                const auto index = restored.session.completed_tick();
                expect(take(restored.session.step(pool)).state_hash.get() == hashes[index], "1/2/4/8 parity and no double payment after load");
            }
        }
    }
}
}
int case_galactic_fiscal_tests() {
    try { runtime_saturation(); retail_c01(); contribution_math(); unclamped_alignment(); trade_and_alignment(); malformed_static_tail(); preview(); preview(true); accounts(); signed_planet_income(); boundaries(); ownership_at_deadline(); combined_extensions(); replay_and_checkpoint(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures == 0 ? 0 : 1;
}
