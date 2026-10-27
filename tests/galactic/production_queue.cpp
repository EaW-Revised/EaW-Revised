#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include "../support/result.hpp"
#include "production_private.hpp"
#include "galactic_support.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/replay.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "eawr/sim/galactic/fleet_intelligence.hpp"
#include "../../src/sim/galactic/checkpoint_fields_internal.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace galactic_production_test {
using namespace galactic_test;
namespace {
using eawr::testing::result_code_colon::take;
class ProductionCopyProbe final : public eawr::sim::PartitionExecutor {
public:
    explicit ProductionCopyProbe(const eawr::sim::PartitionExecutor& inner, bool fail = false)
        : inner_(inner), fail_(fail) {}
    std::size_t worker_count() const noexcept override { return inner_.worker_count(); }
    eawr::core::Result<void> execute(std::size_t partitions,
        const std::function<void(std::size_t)>& work) const override { return inner_.execute(partitions, work); }
    eawr::core::Result<void> execute_phase(std::string_view phase, std::size_t partitions,
        const std::function<void(std::size_t)>& work) const override {
        if (phase == "galactic-production-snapshot") {
            copies.push_back(partitions);
            if (fail_) {
                work(partitions - 1); // Leave genuinely partial staging, including the final planet.
                eawr::core::Diagnostic error; error.message = "synthetic production copy failure";
                return eawr::core::Result<void>::failure(std::move(error));
            }
        }
        return inner_.execute_phase(phase, partitions, work);
    }
    mutable std::vector<std::size_t> copies;
private:
    const eawr::sim::PartitionExecutor& inner_;
    bool fail_;
};
} // namespace

Fixed f(std::int64_t value) { return take(Fixed::from_integer(value)); }

galactic::GalacticSetup setup() {
    galactic::SyntheticGalaxyOptions options; options.players = 1; options.planets = 4;
    options.fleets_per_player = 1; options.members_per_fleet = 1; options.debug_commands = true;
    auto config = galactic::synthetic_galaxy(options);
    config.rules.maximum_fleet_movement_distance = f(10000); config.rules.fiscal_cycle_frames = 3;
    config.fleet_control = galactic::FleetControlSetup{{}, {{1000, false, true, false}}, {}};
    config.player_economy = galactic::PlayerEconomySetup{{{1, f(250), f(10000), 2, 5}}};
    config.fiscal = galactic::FiscalSetup{f(2500), {{1, true, f(1)}}, {}};
    config.production = galactic::ProductionSetup{5, {{1, true, false, true, f(50), f(0), {}, {}, {}}}, {}};
    for (const auto& planet : config.planets) {
        config.fiscal->planets.push_back({planet.id, f(10)});
        galactic::PlanetProduction production;
        production.planet = planet.id; production.target_owner = 1; production.starbase_owner = 1;
        production.ground_occupier = 1; production.space_level = 1; production.maximum_space_level = 5;
        production.ground_slots = 3; production.space_slots = 3; production.maximum_ground_forces = 10;
        production.enabled = true; production.surface_accessible = true;
        galactic::ProductionOffer type; type.type = 1000; type.price = type.authored_price = f(100);
        type.seconds = take(Fixed::from_decimal("0.1")); type.tech = 2; type.population = f(1);
        type.affiliated = true; type.direct_build_allowed = true;
        production.offers.push_back(type);
        type.type = 1001; type.kind = galactic::ProductionKind::structure; type.population = f(0);
        production.offers.push_back(type);
        config.production->planets.push_back(production);
    }
    return config;
}

galactic::GameCommand buy(std::uint64_t tick, std::uint64_t sequence, galactic::PlanetId planet,
    galactic::TypeId type, galactic::ProductionDomain domain) {
    return {{tick, {1}, sequence}, galactic::ProductionCommand{planet, type, galactic::ProductionAction::buy, domain, 0}};
}

galactic::GameCommand cancel(std::uint64_t tick, std::uint64_t sequence, std::uint32_t index,
    galactic::TypeId type, galactic::ProductionAction action) {
    return {{tick, {1}, sequence}, galactic::ProductionCommand{1, type, action, galactic::ProductionDomain::ground, index}};
}

void submit(galactic::GalacticSession& game, galactic::GameCommand command) {
    if (auto result = game.submit(command); !result) throw std::runtime_error(result.error().message);
}

void admission() {
    eawr::sim::InlineExecutor serial; auto config = setup();
    auto game = take(galactic::GalacticSession::create(config));
    submit(game, buy(0, 1)); submit(game, buy(0, 2, 2)); submit(game, buy(0, 3, 3));
    const auto tick = take(game.step(serial));
    expect(tick.work.production_producers == 2 && tick.work.production_entries == 2, "work is bounded by active queues, not all planets/offers");
    expect(game.snapshot()->player_economy()->players[0].credits == f(50), "two planets atomically share the account");
    expect(game.snapshot()->production()->planets[2].queues[0].empty(), "third buy cannot overspend");
    expect(tick.snapshot->events().back().reason == galactic::GalacticRejectReason::production_admission, "typed admission rejection");
    // LSA-04/06: signed integer limits below -1 remain valid at session creation.
    for (const auto limit : {-2LL, -2147483648LL}) {
        auto input = setup();
        auto& offer = input.production->planets[0].offers[1];
        offer.planet_limit = offer.current_limit = offer.lifetime_limit = limit;
        const auto created = galactic::GalacticSession::create(input);
        expect(static_cast<bool>(created), "signed negative limits survive production validation");
        if (created) {
            const auto& producer = input.production->planets[0];
            const auto admitted = galactic::production_admission({*input.production, producer,
                input.production->players[0], input.player_economy->players[0], input.fleets,
                input.planets[0].owner, nullptr, 10000, 1}, &offer, galactic::ProductionDomain::ground);
            expect(admitted == galactic::ProductionAdmission::allowed, "every negative signed limit disables its cap");
        }
    }
    {
        auto input = setup(); auto& offer = input.production->planets[0].offers[0];
        offer.kind = galactic::ProductionKind::technology; offer.target_tech = 4294967295LL;
        expect(static_cast<bool>(galactic::GalacticSession::create(input)), "LSA-08 uint32 technology target survives session creation");
        const auto& producer = input.production->planets[0];
        const auto admitted = galactic::production_admission({*input.production, producer,
            input.production->players[0], input.player_economy->players[0], input.fleets,
            input.planets[0].owner, nullptr, 10000, 1}, &offer, galactic::ProductionDomain::ground);
        expect(admitted == galactic::ProductionAdmission::tech, "wrapped technology target is unavailable above the account maximum");
    }
    const auto check = [&](auto mutate, galactic::ProductionAdmission why, galactic::TypeId type = 1000) {
        auto input = setup(); mutate(input); auto session = take(galactic::GalacticSession::create(input));
        const auto& producer = input.production->planets[0];
        const auto offer = std::find_if(producer.offers.begin(), producer.offers.end(),
            [type](const auto& row) { return row.type == type; });
        const auto query = galactic::production_admission({*input.production, producer,
            input.production->players[0], input.player_economy->players[0], input.fleets,
            input.planets[0].owner, nullptr, 10000, 1}, &*offer, galactic::ProductionDomain::ground);
        expect(query == why, "read-only admission agrees with enqueue rejection");
        submit(session, buy(0, 1, 1, type)); auto result = take(session.step(serial));
        expect(result.snapshot->production()->planets[0].queues[0].empty(), "failed admission leaves queue empty");
        expect(result.snapshot->player_economy()->players[0].credits == f(250), "failed admission leaves ledger unchanged");
        expect(result.snapshot->events().back().value == static_cast<std::uint64_t>(why), "correct admission detail");
    };
    check([](auto& s) { s.player_economy->players[0].current_tech = 1; }, galactic::ProductionAdmission::tech);
    check([](auto& s) { s.production->players[0].locked = {1000}; }, galactic::ProductionAdmission::locked);
    check([](auto& s) { s.production->players[0].population_used = f(60); }, galactic::ProductionAdmission::population);
    check([](auto& s) { s.production->planets[0].ground_slots = 0; }, galactic::ProductionAdmission::slots, 1001);
    check([](auto& s) { s.production->planets[0].offers[0].current_limit = 0; }, galactic::ProductionAdmission::current_limit);
    check([](auto& s) { s.production->planets[0].offers[0].lifetime_limit = 0; }, galactic::ProductionAdmission::lifetime_limit);
    check([](auto& s) { s.production->planets[0].offers[0].required_structures = {{1001}}; }, galactic::ProductionAdmission::prerequisites);
    // LSA-09: requirements are source integers, not a campaign-load level bound.
    for (const auto level : {0LL, 5LL, 6LL, 99LL, -1LL, -2147483648LL, 2147483647LL}) {
        for (const bool ground : {true, false}) {
            auto input = setup(); auto& planet = input.production->planets[0];
            planet.ground_level = planet.space_level = 2;
            auto& offer = planet.offers[0];
            offer.domain = ground ? galactic::ProductionDomain::ground : galactic::ProductionDomain::space;
            (ground ? offer.required_ground_level : offer.required_space_level) = level;
            galactic::GalacticReplay replay; replay.setup = input;
            expect(take(galactic::parse_replay(take(galactic::write_replay(replay)))) == replay,
                "signed and oversized requirements survive replay encoding");
            std::string baseline;
            for (const auto workers : eawr::platform::determinism_worker_counts()) {
                eawr::platform::ThreadWorkerAdapter pool(workers);
                auto session = take(galactic::GalacticSession::create(input));
                const auto saved = take(session.save_checkpoint_state());
                auto restored = take(galactic::GalacticSession::create(input));
                (void)take(restored.load_checkpoint_state(saved));
                expect(take(restored.save_checkpoint_state()) == saved, "requirement checkpoint is canonical");
                const auto order = buy(0, 1, 1, 1000, offer.domain);
                submit(session, order); submit(restored, order);
                const auto result = take(session.step(pool));
                expect(take(restored.step(pool)).state_hash.get() == result.state_hash.get(),
                    "requirement checkpoint continuation matches");
                if (baseline.empty()) baseline = result.state_hash.get();
                expect(result.state_hash.get() == baseline, "requirement admission agrees on 1/2/4/8 workers");
                const bool blocked = level != 0;
                const auto channel = ground ? 0U : 1U;
                expect(result.snapshot->production()->planets[0].queues[channel].empty() == blocked,
                    "unsigned requirement gates enqueue against the actual base");
                expect(result.snapshot->player_economy()->players[0].credits == f(blocked ? 250 : 150),
                    "unreachable requirement never debits credits");
                if (blocked) expect(result.snapshot->events().back().value
                    == static_cast<std::uint64_t>(galactic::ProductionAdmission::prerequisites),
                    "unreachable requirement reports prerequisites rather than aborting load");
            }
        }
    }
    for (const auto level : {-2147483649LL, 2147483648LL}) {
        for (const bool ground : {true, false}) {
            auto input = setup(); auto& offer = input.production->planets[0].offers[0];
            (ground ? offer.required_ground_level : offer.required_space_level) = level;
            expect(!galactic::GalacticSession::create(input), "requirements outside source int32 remain invalid");
        }
    }
    for (const bool ground : {true, false}) {
        for (const auto level : {1LL, 99LL, -1LL, -2147483648LL}) {
            std::string baseline;
            for (const auto workers : eawr::platform::determinism_worker_counts()) {
                eawr::platform::ThreadWorkerAdapter pool(workers);
                auto input = setup(); auto& planet = input.production->planets[0];
                planet.ground_level = planet.space_level = 2;
                auto& offer = planet.offers[0]; offer.seconds = f(100);
                offer.domain = ground ? galactic::ProductionDomain::ground : galactic::ProductionDomain::space;
                (ground ? offer.required_ground_level : offer.required_space_level) = 1;
                auto session = take(galactic::GalacticSession::create(input));
                submit(session, buy(0, 1, 1, 1000, offer.domain));
                (void)take(session.step(pool));
                auto original = session.snapshot()->production()->planets[0];
                const auto channel = ground ? 0U : 1U;
                expect(original.queues[channel].size() == 1, "service fixture starts with admitted work");
                auto changed = original;
                (ground ? changed.ground_level : changed.space_level) = 0;
                (ground ? changed.offers[0].required_ground_level : changed.offers[0].required_space_level) = level;
                // Restore a synthetic queued owner after losing its prerequisite.
                // Signed/unreachable requirements cannot normally enter via purchase.
                galactic::checkpoint_detail::Writer before, after;
                before(original); after(changed);
                auto saved = take(session.save_checkpoint_state());
                const auto at = std::search(saved.begin(), saved.end(), before.bytes.begin(), before.bytes.end());
                if (at == saved.end() || before.bytes.size() != after.bytes.size())
                    throw std::runtime_error("queued producer checkpoint fixture not found");
                std::copy(after.bytes.begin(), after.bytes.end(), at);
                auto restored = take(galactic::GalacticSession::create(input));
                (void)take(restored.load_checkpoint_state(saved));
                const auto balance = restored.snapshot()->player_economy()->players[0].credits;
                const auto result = take(restored.step(pool));
                expect(result.snapshot->production()->planets[0].queues[channel].empty(),
                    "queue service cancels work with an unreachable unsigned prerequisite");
                expect(result.snapshot->player_economy()->players[0].credits == balance,
                    "lost prerequisite neither bills nor directly refunds queued work");
                expect(std::ranges::any_of(result.snapshot->events(), [](const auto& event) {
                    return event.kind == galactic::GalacticEventKind::production_cancelled;
                }), "lost prerequisite emits queue cancellation");
                if (baseline.empty()) baseline = result.state_hash.get();
                expect(result.state_hash.get() == baseline, "prerequisite servicing agrees on 1/2/4/8 workers");
            }
        }
    }
    auto zero_pop = setup(); zero_pop.production->players[0].population_used = f(60);
    auto structures = take(galactic::GalacticSession::create(zero_pop)); submit(structures, buy(0, 1, 1, 1001));
    (void)take(structures.step(serial)); expect(structures.snapshot()->production()->planets[0].queues[0].size() == 1, "zero-pop structure over cap allowed");
}

void offer_projection() {
    auto config = setup();
    // Foreign observer facts still require a fully listed faction provider.
    config.players.push_back({2, 2, 0});
    config.player_economy->players.push_back({2, f(250), f(10000), 2, 5});
    config.fiscal->players.push_back({2, true, f(1)});
    config.production->players.push_back({2, false, true, false, f(50), f(0), {}, {}, {}});
    galactic::ObserverSetup observer;
    observer.human_observers = {1}; observer.solo_reference_human = 1; observer.reveal_delay_seconds = f(1);
    observer.players = {{1, false, false, 0, 1, {}}, {2, true, false, 0, 1, {}}};
    observer.types = {{1000, false, false, false}};
    for (const auto& planet : config.planets) {
        galactic::ObserverPlanet facts;
        facts.planet = planet.id; facts.allegiance = facts.target_owner = 1;
        facts.current_owner = facts.current_controller = 1;
        observer.planets.push_back(facts);
        observer.records.push_back({1, planet.id, true, 0, 0, 0, {}});
    }
    // Nonlocal and undiscovered payloads are deliberately still populated.
    observer.planets[1].allegiance = observer.planets[1].current_controller = 2;
    observer.records[2].discovered = false;
    for (const auto& planet : config.planets)
        observer.records.push_back({2, planet.id, true, 0, 0, 0, {}});
    config.observer = observer;
    std::vector<galactic::ObservedProductionOffer> baseline;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(galactic::GalacticSession::create(config));
        const auto initial = game.snapshot();
        const auto offers = initial->production_offers();
        expect(offers.size() == 4, "only discovered local-controlled producer offers published");
        for (const auto& row : offers) {
            expect(row.player == 1 && (row.planet == 1 || row.planet == 4), "secret producers have no rows");
            expect(row.current && row.admission == galactic::ProductionAdmission::allowed && row.price == f(100), "query publishes command price and list membership");
            expect(!row.eventual.has_value(), "unsourced eventual semantics remain unknown");
            expect(row.base_price == f(100), "authored price published beside the command price (GHUD-S57)");
            expect(row.population_fits, "room for the population is published (GHUD-S57)");
        }
        submit(game, buy(0, 1)); const auto tick = take(game.step(pool));
        expect(tick.work.observer_projection_records == observer.records.size(),
            "production offer publication rebuilds the observer projection only once");
        const auto fresh = galactic::observed_planets(*tick.snapshot->observer(),
            tick.snapshot->fleets(), {}, tick.snapshot->movement());
        expect(std::ranges::equal(tick.snapshot->observations(), fresh),
            "production admission consumes the current observer projection");
        const auto after = game.snapshot()->production_offers();
        expect(after.front().already_in_production, "queued type presence published");
        expect(initial->production_offers().front().already_in_production == false, "older publication immutable");
        if (baseline.empty()) baseline.assign(after.begin(), after.end());
        else expect(std::equal(after.begin(), after.end(), baseline.begin(), baseline.end()), "offer projection equal on 1/2/4/8 workers");
    }
    {
        auto multiplayer = config;
        multiplayer.seats.push_back({{2}, 2, galactic::SeatKind::human, 1, true, 2});
        multiplayer.observer->human_observers = {1, 2}; multiplayer.observer->solo_reference_human = 0;
        multiplayer.production->players[1].human = true;
        multiplayer.production->players[1].pay_as_you_go = false;
        multiplayer.production->players[1].uses_population_cap = true;
        multiplayer.observer->players[1].ai = false;
        multiplayer.planets[1].owner = 2;
        auto& foreign = multiplayer.production->planets[1];
        foreign.target_owner = foreign.starbase_owner = foreign.ground_occupier = 2;
        auto& facts = multiplayer.observer->planets[1];
        facts.allegiance = facts.target_owner = facts.current_owner = facts.current_controller = 2;
        auto game = take(galactic::GalacticSession::create(multiplayer));
        const auto canonical = game.snapshot()->sha256();
        const auto host = game.view_snapshot({1}); const auto guest = game.view_snapshot({2});
        expect(!host->production_offers().empty() && !guest->production_offers().empty(),
            "both human contexts receive their own production menus");
        for (const auto& row : host->production_offers()) expect(row.player == 1 && row.planet != 2,
            "host menu excludes the guest's secret producer");
        for (const auto& row : guest->production_offers()) expect(row.player == 2 && row.planet == 2,
            "guest menu contains only its admitted producer");
        expect(game.view_snapshot({1})->production_offers().data() == host->production_offers().data()
            && game.view_snapshot({2})->production_offers().data() == guest->production_offers().data()
            && game.snapshot()->sha256() == canonical,
            "alternating menu queries reuse player caches without changing canonical state");
    }
    {
        auto active = config;
        active.rules.fiscal_cycle_frames = 1000;
        active.planets[1].owner = 2;
        auto& foreign = active.production->planets[1];
        foreign.target_owner = foreign.starbase_owner = foreign.ground_occupier = 2;
        for (auto& planet : active.production->planets) {
            planet.offers.front().seconds = f(2);
            // Large immutable content makes the encoded-byte budget observable without timing.
            for (galactic::TypeId id = 2000; id < 2064; ++id) {
                auto type = planet.offers.back(); type.type = id;
                planet.offers.push_back(std::move(type));
            }
        }
        active.production->technology_types = {{1000, 2, false, false, {1, 2}}, {2064, 5, true, true, {2}}};
        active.production->fleet_blockers.emplace(); // Preserve present-empty separately from absence.
        active.production->planets.front().offers.back().required_structures = {{1000, 1001}, {1001}};
        active.production->planets.front().offers.back().required_orbiting_units = {1000};
        std::vector<std::string> hashes;
        for (auto workers : eawr::platform::determinism_worker_counts()) {
            eawr::platform::ThreadWorkerAdapter pool(workers);
            auto game = take(galactic::GalacticSession::create(active));
            game.set_hash_verification(true);
            game.set_native_production_handler([](const auto& notice) {
                return eawr::core::Result<galactic::GalacticSession::NativeProductionPayment>::success(
                    {true, notice.player, notice.amount});
            });
            const auto admitted = take(game.admit_native_production({2, 2, 1000, f(100)}, pool));
            expect(admitted.admission == galactic::ProductionAdmission::allowed, "foreign AI production starts");
            submit(game, buy(0, 1));
            auto previous = take(game.step(pool));
            const auto canonical_size = game.canonical_state_bytes().size();
            for (std::size_t tick = 0; tick < 6; ++tick) {
                ProductionCopyProbe copy_probe(pool);
                const auto result = take(game.step(copy_probe));
                expect(copy_probe.copies == std::vector<std::size_t>{eawr::sim::tick_partition_count},
                    "one dirty production publication stages exactly one fixed partition set");
                expect(result.snapshot->production()->technology_types == active.production->technology_types
                    && result.snapshot->production()->fleet_blockers.has_value()
                    && result.snapshot->production()->fleet_blockers->empty(),
                    "partitioned copy retains nested technology affiliations and present-empty blockers");
                expect(result.snapshot->production()->planets.front().offers
                    == previous.snapshot->production()->planets.front().offers,
                    "changed queue snapshots retain every nested immutable offer prerequisite");
                expect(result.work.production_entries == 2, "both local and AI queues make progress");
                expect(result.work.production_hash_bytes > 0 && result.work.production_hash_bytes < canonical_size / 4,
                    "dirty production hashes mutable state and fixed-size digests, excluding immutable offer bytes");
                expect(result.work.production_offer_rebuilds == 0,
                    "active countdowns and AI payments reuse local offer rows");
                expect(result.snapshot->production_offers().data() == previous.snapshot->production_offers().data(),
                    "active ticks retain the immutable offer publication");
                expect(result.snapshot->player_economy()->players[1].credits < previous.snapshot->player_economy()->players[1].credits,
                    "AI bills really change its account on each observed tick");
                expect(result.snapshot->production()->planets.front().queues[0].front().last_remaining
                    < previous.snapshot->production()->planets.front().queues[0].front().last_remaining,
                    "human countdown really changes despite reused offer rows");
                if (workers == 1) hashes.push_back(result.state_hash.get());
                else expect(result.state_hash.get() == hashes[tick], "active production hashes agree across worker counts");
                previous = result;
            }
            // A membership change still invalidates the rows immediately.
            submit(game, cancel(game.completed_tick(), 2, 0));
            const auto cancelled = take(game.step(pool));
            expect(cancelled.work.production_offer_rebuilds == 1
                && !cancelled.snapshot->production_offers().front().already_in_production,
                "cancellation invalidates membership and refund admission inputs");
        }
        auto changed = active;
        changed.production->planets.front().offers.back().authored_price = f(123);
        auto original = take(galactic::GalacticSession::create(active));
        auto different = take(galactic::GalacticSession::create(changed));
        expect(original.state_sha256() != different.state_sha256(), "immutable offer content remains part of state identity");
        eawr::sim::InlineExecutor serial;
        // Direct restore accepts fresh owners. Prime the target's immutable digest through the hash API first.
        expect(original.cached_state_hash().get() != different.cached_state_hash().get(),
            "fresh owners cache different immutable offer digests");
        (void)take(original.step(serial));
        const auto saved = take(original.save_checkpoint_state());
        const auto loaded = different.load_checkpoint_state(saved);
        if (!loaded) throw std::runtime_error(loaded.error().message);
        auto fresh = take(galactic::GalacticSession::create(active));
        const auto restored = fresh.load_checkpoint_state(saved);
        if (!restored) throw std::runtime_error(restored.error().message);
        expect(different.cached_state_hash().get() == fresh.state_sha256(),
            "in-place checkpoint load replaces cached immutable offer digests with the restored owner's content");
    }
    {
        // WGUI-170: renamed/synthetic ground, space and station offers retain
        // their identity and price while fiscal income changes admission.
        auto poor = config;
        poor.player_economy->players[0].credits = f(50);
        poor.fleet_control->types.push_back({1002, false, true, false});
        for (auto& planet : poor.production->planets) {
            auto type = planet.offers.front();
            type.type = 1002; type.domain = galactic::ProductionDomain::space;
            planet.offers.push_back(type);
            type.type = 1003; type.kind = galactic::ProductionKind::starbase;
            type.population = {}; type.base_level = 2;
            planet.offers.push_back(type);
            type.type = 1004; type.kind = galactic::ProductionKind::structure;
            planet.offers.push_back(type);
        }
        std::vector<galactic::ObservedProductionOffer> reference;
        std::vector<std::string> hashes;
        for (auto workers : eawr::platform::determinism_worker_counts()) {
            eawr::platform::ThreadWorkerAdapter pool(workers);
            auto game = take(galactic::GalacticSession::create(poor));
            const auto initial = game.snapshot();
            expect(initial->production_offers().size() == 10, "both observed producers retain all five poor offers");
            for (const auto& row : initial->production_offers())
                expect(row.current && row.admission == galactic::ProductionAdmission::credits
                    && row.price == f(100), "poor offer stays listed, disabled, with its cost");
            submit(game, buy(0, 1));
            auto previous_credits = initial->player_economy()->players.front().credits;
            std::uint64_t rebuild_ticks{};
            for (std::uint64_t tick = 0; tick < 7; ++tick) {
                const auto result = take(game.step(pool));
                // The offer rows are rebuilt exactly when a state section they read changed: an idle tick reuses them.
                const auto credits = result.snapshot->player_economy()->players.front().credits;
                if (credits == previous_credits)
                    expect(result.work.production_offer_rebuilds == 0 || tick == 0, "unchanged inputs reuse the published offer rows");
                else expect(result.work.production_offer_rebuilds == 1, "a credit change rebuilds the offer rows once");
                rebuild_ticks += result.work.production_offer_rebuilds;
                previous_credits = credits;
                if (workers == 1) hashes.push_back(result.state_hash.get());
                else expect(result.state_hash.get() == hashes[static_cast<std::size_t>(tick)], "income and refusals equal on 1/2/4/8");
                const bool funded = result.snapshot->player_economy()->players.front().credits >= f(100);
                for (const auto& row : result.snapshot->production_offers())
                    expect(row.current && row.price == f(100)
                        && row.admission == (funded ? galactic::ProductionAdmission::allowed : galactic::ProductionAdmission::credits),
                        "credits arriving re-enable the same list entries at the fiscal boundary");
            }
            expect(rebuild_ticks < 7, "idle ticks of the poor scenario reuse the offer rows");
            expect(game.snapshot()->production()->planets.front().queues[0].empty(), "poor direct purchase neither queues nor debits");
            expect(game.snapshot()->player_economy()->players.front().credits == f(130), "only income changed the poor account");
            const auto rows = game.snapshot()->production_offers();
            if (reference.empty()) reference.assign(rows.begin(), rows.end());
            else expect(std::ranges::equal(rows, reference), "list facts equal on 1/2/4/8");
            expect(initial->production_offers().front().admission == galactic::ProductionAdmission::credits, "retained poor publication stays immutable");
        }
        const auto hidden = [&](auto mutate, galactic::TypeId type = 1000) {
            auto blocked = poor; mutate(blocked);
            auto game = take(galactic::GalacticSession::create(blocked));
            const auto rows = game.snapshot()->production_offers();
            const auto found = std::ranges::find_if(rows, [type](const auto& row) { return row.planet == 1 && row.type == type; });
            expect(found != rows.end(), "secondary blocker has an observed offer");
            if (found == rows.end()) return;
            const auto& row = *found;
            expect(row.admission == galactic::ProductionAdmission::credits && !row.current,
                "credits never conceal a secondary list blocker");
        };
        hidden([](auto& s) { s.player_economy->players[0].current_tech = 1; });
        hidden([](auto& s) { s.production->players[0].locked = {1000}; });
        hidden([](auto& s) { s.production->planets[0].offers[0].required_structures = {{1001}}; });
        hidden([](auto& s) { s.production->planets[0].offers[0].current_limit = 0; });
        hidden([](auto& s) { s.production->planets[0].offers[0].lifetime_limit = 0; });
        hidden([](auto& s) { s.production->planets[0].offers[0].kind = galactic::ProductionKind::structure;
            s.production->planets[0].ground_slots = 0; });
        hidden([](auto& s) { s.production->planets[0].offers[1].planet_limit = 0; }, 1001);
        hidden([](auto& s) { s.production->planets[0].offers[3].base_level = 3; }, 1003);
        hidden([](auto& s) { s.production->planets[0].maximum_space_level = 1; }, 1003);
        hidden([](auto& s) { s.production->planets[0].target_owner = 2; });
        hidden([](auto& s) { s.production->planets[0].starbase_owner = 2; }, 1002);
        hidden([](auto& s) { s.production->planets[0].offers[0].required_ground_level = 1; });
        hidden([](auto& s) { s.production->planets[0].offers[2].required_space_level = 2; }, 1002);
        auto bounded = config;
        bounded.fleets.front().id = std::numeric_limits<eawr::sim::EntityId>::max() - 3;
        auto bounded_game = take(galactic::GalacticSession::create(bounded));
        expect(bounded_game.snapshot()->production_offers().size() == 4, "ID-bound scenario retains observed choices");
        for (const auto& row : bounded_game.snapshot()->production_offers())
            expect(row.current && row.admission == galactic::ProductionAdmission::resource_limit,
                "project ID bound disables a capable listed offer");
        auto full = config;
        full.production->maximum_human_queue = 1;
        full.player_economy->players[0].credits = f(150);
        auto game = take(galactic::GalacticSession::create(full));
        eawr::sim::InlineExecutor serial;
        submit(game, buy(0, 1, 1, 1001)); (void)take(game.step(serial));
        expect(game.snapshot()->production_offers().front().current, "full queue plus low funds keeps otherwise capable type listed");
        auto station_slot = poor;
        station_slot.player_economy->players[0].credits = f(150);
        auto other_station = station_slot.production->planets[0].offers[3];
        other_station.type = 1005;
        station_slot.production->planets[0].offers.push_back(other_station);
        auto stations = take(galactic::GalacticSession::create(station_slot));
        submit(stations, buy(0, 1, 1, 1003, galactic::ProductionDomain::space));
        (void)take(stations.step(serial));
        const auto station_rows = stations.snapshot()->production_offers();
        const auto other = std::ranges::find_if(station_rows, [](const auto& row) { return row.type == 1005; });
        expect(other != station_rows.end() && !other->current && !other->already_in_production
            && other->admission == galactic::ProductionAdmission::credits,
            "an occupied starbase queue slot hides the other station type despite low funds");
    }
    {
        // GHUD-S57: admission reports credits before population; the population fit is published on its own.
        auto poor = config;
        poor.player_economy->players[0].credits = f(50);
        const auto rows = [](const galactic::GalacticSetup& built) {
            auto game = take(galactic::GalacticSession::create(built));
            const auto snapshot = game.snapshot(); const auto view = snapshot->production_offers();
            return std::vector<galactic::ObservedProductionOffer>(view.begin(), view.end());
        };
        auto offers = rows(poor);
        expect(offers.size() == 4 && std::ranges::all_of(offers, [](const auto& row) {
            return row.current && row.admission == galactic::ProductionAdmission::credits && row.population_fits; }),
            "credits short, population room: the credits reason and a population fit");
        poor.production->players[0].population_used = f(50); // at the cap: type 1000 needs 1, type 1001 needs none
        offers = rows(poor);
        expect(offers.size() == 4 && std::ranges::all_of(offers, [](const auto& row) {
            return row.current && row.admission == galactic::ProductionAdmission::credits && row.population_fits == (row.type == 1001); }),
            "credits and population short: admission still says credits, the population fit says no");
        // A mod's non-1.0 uncorrupted factor is already inside the offer price; the base stays the authored cost.
        auto raised = config;
        for (auto& planet : raised.production->planets) planet.offers[0].price = f(120);
        offers = rows(raised);
        expect(std::ranges::any_of(offers, [](const auto& row) { return row.type == 1000; })
            && std::ranges::all_of(offers, [](const auto& row) {
                return row.type != 1000 || (row.price == f(120) && row.base_price == f(100)); }),
            "scaled price published against the unscaled authored cost");
    }
    config.observer.reset();
    expect(take(galactic::GalacticSession::create(config)).snapshot()->production_offers().empty(), "missing observer never invents offer knowledge");

    galactic::IncomeSetup income;
    galactic::EconomicType source; source.type = 900;
    source.reductions = {{1000, 1, take(Fixed::from_decimal("0.5")), true, false}};
    income.types = {source}; income.sources = {{1, 900, 1, 1, true, true}};
    for (const auto& planet : config.planets) income.planets.push_back({planet.id, false, false, false});
    income.offers = {{1, 1000, f(101), f(10)}};
    const auto adjusted = take(galactic::prepared_production_offer(config.production->planets[0].offers[0],
        &income, config.planets, 1, 1));
    expect(adjusted.price == f(51) && adjusted.seconds == f(10), "income query rounds once after location reduction");
    expect(!galactic::prepared_production_offer(config.production->planets[0].offers[1], &income,
        config.planets, 1, 1), "missing income basis cannot publish guessed price");
}

// WGEP-40/WGEP-66, GQP-11: a completed orbit unit is its own fleet at slot 0 and
// is not merged into the planet's existing slot-0 fleet. The observer projection
// must still show it: one slot card whose count covers both fleets.
void orbit_completion_shares_slot_card() {
    auto config = setup();
    config.production->planets[0].offers[0].domain = galactic::ProductionDomain::space;
    galactic::ObserverSetup observer;
    observer.human_observers = {1}; observer.solo_reference_human = 1; observer.reveal_delay_seconds = f(1);
    observer.players = {{1, false, false, 0, 1, {}}};
    observer.types = {{1000, false, false, false}};
    for (const auto& planet : config.planets) {
        galactic::ObserverPlanet facts;
        facts.planet = planet.id; facts.allegiance = facts.target_owner = 1;
        facts.current_owner = facts.current_controller = 1;
        observer.planets.push_back(facts);
        observer.records.push_back({1, planet.id, true, 0, 0, 0, {}});
    }
    config.observer = observer;
    const std::vector<galactic::FleetDisplayType> traits{{1000, false, false, false, false, {}, 0}};
    const std::vector<galactic::FleetDisplayVisibility> sight{{1, true}};
    std::optional<galactic::FleetIntelligence> baseline;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(galactic::GalacticSession::create(config));
        const auto before = galactic::observed_fleets(*game.snapshot(), 1, traits, 8, sight);
        expect(before.cards.size() == 1 && before.cards[0].count == 1 && before.cards[0].orbit_slot == 0,
            "starting fleet publishes a slot-0 card");
        submit(game, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
        for (int i = 0; i < 4; ++i) (void)take(game.step(pool));
        const auto fleets = game.snapshot()->fleets();
        expect(fleets.size() == 2 && fleets[0].orbit == 1 && fleets[1].orbit == 1
            && fleets[0].orbit_slot == 0 && fleets[1].orbit_slot == 0 && fleets[0].members.size() == 1
            && fleets[1].members.size() == 1, "completed unit is a new slot-0 fleet beside the existing one");
        const auto after = galactic::observed_fleets(*game.snapshot(), 1, traits, 8, sight);
        expect(after.cards.size() == 1 && after.cards[0].fleet == fleets[0].id && after.cards[0].count == 2
            && after.cards[0].groups.size() == 1 && after.cards[0].groups[0].count == 2,
            "purchased ship is counted on the slot-0 card instead of hidden");
        if (!baseline) baseline = after;
        else expect(*baseline == after, "slot card equal on 1/2/4/8 workers");
    }
}

void technology_price_production() {
    auto config = setup();
    galactic::EconomyCorruptionState corruption;
    galactic::CorruptionEconomyPlayer player; player.player = 1; player.faction = 1; player.playable = true;
    player.price = {f(1), take(Fixed::from_decimal("0.9"))};
    player.time = {f(1), take(Fixed::from_decimal("0.95"))};
    player.technology_price_modifiers = {{900, take(Fixed::from_decimal("-0.5")), 2}};
    player.tech_purchase_multiplier = take(Fixed::from_decimal("0.5"));
    corruption.players = {player};
    for (const auto& planet : config.planets) corruption.planets.push_back({planet.id, {}, false, false, galactic::corruption_fixture::one, {}, {}, {}});
    auto research = config.production->planets[0].offers[0];
    research.price = f(101); research.seconds = f(10);
    research.kind = galactic::ProductionKind::technology; research.target_tech = 3; research.population = {};
    galactic::IncomeSetup basis; basis.offers = {{1, research.type, f(101), f(10)}};
    const auto prepare = [&](galactic::ProductionOffer original, const galactic::IncomeSetup* income) {
        return take(galactic::prepared_production_offer(original, income, config.planets, 1, 1, &corruption));
    };
    expect(prepare(research, &basis).price == f(51) && prepare(research, &basis).seconds == f(10),
        "research-only price factor precedes rounding and leaves duration intact");
    auto ordinary = research; ordinary.kind = galactic::ProductionKind::unit; ordinary.target_tech = 0;
    expect(prepare(ordinary, &basis).price == f(101), "ordinary production ignores technology-price factor");
    corruption.planets[0].corruption = galactic::corruption_fixture::Type::racketeering;
    // The authored decimal factor is represented in Q24 before multiplication;
    // duration retains that exact result instead of rounding back to decimal.
    const auto reduced_seconds = Fixed::from_raw(10 * player.time[1].raw());
    expect(prepare(research, &basis).price == f(45) && prepare(research, nullptr).price == f(45)
        && prepare(research, &basis).seconds == reduced_seconds,
        "corruption and technology factors combine before a single rounding step");
    corruption.players[0].tech_purchase_multiplier = take(Fixed::from_decimal("0.0001"));
    expect(prepare(research, &basis).price == f(1), "sourced fallback keeps production positive subcredit minimum");
    corruption = {}; corruption.players = {player};
    for (const auto& planet : config.planets) corruption.planets.push_back({planet.id, {}, false, false, galactic::corruption_fixture::one, {}, {}, {}});
    config.economy_corruption = corruption;
    for (auto& planet : config.production->planets) {
        auto& offer = planet.offers[0]; offer = research; offer.price = f(100); offer.seconds = f(100);
    }
    eawr::sim::InlineExecutor executor;
    auto game = take(galactic::GalacticSession::create(config));
    submit(game, buy(0, 1)); (void)take(game.step(executor));
    expect(game.snapshot()->production()->planets[0].queues[0].front().price == f(50)
        && game.snapshot()->player_economy()->players[0].credits == f(200), "research purchase records the shared factor once");
    (void)take(game.step(executor));
    expect(game.economy_corruption()->players[0].tech_purchase_multiplier == f(1)
        && game.snapshot()->production()->planets[0].queues[0].front().price == f(50), "expiry leaves stored queue price intact");
    submit(game, cancel(2, 2, 0)); (void)take(game.step(executor));
    expect(game.snapshot()->player_economy()->players[0].credits == f(250), "cancel refunds stored discounted price after expiry");
}

void cancel_identity() {
    auto old = cancel(0, 1, 1);
    const auto legacy = galactic::encode_payload(old.payload);
    expect(legacy.size() == 22, "old production cancel retains its exact encoding");
    const auto decoded = galactic::decode_payload(galactic::CommandLayer::galactic,
        galactic::galactic_opcodes::production, legacy);
    expect(decoded && *decoded == old.payload, "old cancel has no implicit identity guard");
    auto guarded = old;
    std::get<galactic::ProductionCommand>(guarded.payload).target_build_id = 1234;
    const auto bytes = galactic::encode_payload(guarded.payload);
    expect(bytes.size() == 31 && std::equal(legacy.begin(), legacy.end(), bytes.begin()), "versioned identity appends without changing base bytes");
    expect(galactic::decode_payload(galactic::CommandLayer::galactic,
        galactic::galactic_opcodes::production, bytes) == std::optional(guarded.payload), "guarded cancel payload round trip");
    const auto malformed = [&](std::vector<std::uint8_t> input) {
        expect(!galactic::decode_payload(galactic::CommandLayer::galactic,
            galactic::galactic_opcodes::production, input), "malformed cancel identity rejected");
    };
    auto input = bytes; input[22] = 2; malformed(input);
    input = bytes; std::fill(input.begin() + 23, input.end(), std::uint8_t{0}); malformed(input);
    input = bytes; input.pop_back(); malformed(input);
    input = bytes; input.push_back(0); malformed(input);
    input = bytes; input[16] = 0; malformed(input);

    std::string expected_hash;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
    eawr::platform::ThreadWorkerAdapter serial(workers);
    auto config = setup();
    config.player_economy->players[0].credits = f(400);
    auto game = take(galactic::GalacticSession::create(config));
    submit(game, buy(0, 1)); submit(game, buy(0, 2)); (void)take(game.step(serial));
    const auto target = game.snapshot()->production()->planets[0].queues[0][1].build_id;
    for (int i = 0; i < 3; ++i) (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].front().build_id == target, "completed head shifts the displayed target");
    submit(game, buy(4, 3));
    auto stale = cancel(4, 4, 1);
    std::get<galactic::ProductionCommand>(stale.payload).target_build_id = target;
    submit(game, stale); (void)take(game.step(serial));
    const auto& queue = game.snapshot()->production()->planets[0].queues[0];
    expect(queue.size() == 2 && queue[1].build_id != target, "stale same-type indexed cancel cannot remove the replacement");
    expect(game.snapshot()->events().back().value == static_cast<std::uint64_t>(galactic::ProductionAdmission::stale_entry), "stale identity has typed rejection detail");
    submit(game, cancel(5, 5, 1)); (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].size() == 1, "unguarded old cancel preserves its indexed behavior");
    auto correct = cancel(6, 6, 0);
    std::get<galactic::ProductionCommand>(correct.payload).target_build_id = target;
    submit(game, correct); (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].empty(), "matching guarded cancel removes its actual target");
    const auto hash = game.snapshot()->sha256();
    if (expected_hash.empty()) expected_hash = hash;
    else expect(hash == expected_hash, "guarded and legacy cancel sequence equal on 1/2/4/8 workers");
    }

}

void timing_and_cancel() {
    eawr::sim::InlineExecutor serial; auto game = take(galactic::GalacticSession::create(setup()));
    submit(game, buy(0, 1)); submit(game, buy(0, 2)); (void)take(game.step(serial));
    const auto build = game.snapshot()->production()->planets[0].queues[0][1].build_id;
    submit(game, cancel(1, 3, 0, 1001)); (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].size() == 2, "wrong indexed type does not mutate");
    submit(game, cancel(2, 4, 0)); (void)take(game.step(serial));
    expect(game.snapshot()->player_economy()->players[0].credits == f(150), "front cancel refunds stored prepaid price");
    expect(game.snapshot()->production()->planets[0].queues[0].front().build_id == build, "successor retains build identity");
    for (int i = 0; i < 3; ++i) (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].objects.size() == 1, "promoted front receives its full duration");
    expect(game.snapshot()->production()->planets[0].objects[0].paid_cost == f(100), "surface result records paid cost");
}

void inactive_work_budget() {
    eawr::sim::InlineExecutor serial; auto game = take(galactic::GalacticSession::create(setup()));
    (void)take(game.step(serial));
    const auto previous = game.snapshot();
    ProductionCopyProbe probe(serial);
    const auto idle = take(game.step(probe));
    expect(probe.copies.empty() && &*idle.snapshot->production() == &*previous->production(),
        "idle production publishes the same immutable storage without copying rows");
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        ProductionCopyProbe failing(pool, true);
        auto expected = take(galactic::GalacticSession::create(setup()));
        auto fallback = take(galactic::GalacticSession::create(setup()));
        submit(expected, buy(0, 1)); submit(fallback, buy(0, 1));
        for (unsigned tick = 0; tick < 3; ++tick) {
            const auto ordinary = take(expected.step(pool));
            const auto recovered = take(fallback.step(failing));
            expect(ordinary.snapshot->production() == recovered.snapshot->production()
                && ordinary.state_hash.get() == recovered.state_hash.get(),
                "partial failed staging falls back to the complete serial production copy on 1/2/4/8");
        }
        expect(!failing.copies.empty(), "failure coverage reaches a dirty snapshot copy");
    }
    expect(idle.work.production_producers == 0 && idle.work.production_entries == 0, "idle queues run no production phase");
    expect(idle.work.sections_reused >= 8, "idle production reuses its state digest");
}

void refund_recipient() {
#ifdef NDEBUG
    // WGEP-64 release semantics for the debug-invalid builder/recipient mismatch.
    // The debug ownership command reaches this case without mutating queue bytes.
    auto config = setup(); config.rules.fiscal_cycle_frames = 1000;
    config.players.push_back({2, 1, 0});
    config.seats.push_back({{2}, 2, galactic::SeatKind::human, 1, true, 2});
    config.player_economy->players.push_back({2, f(25), f(10000), 2, 5});
    config.fiscal->players.push_back({2, true, f(1)});
    config.production->players.push_back({2, true, false, true, f(50), f(0), {}, {}, {}});
    eawr::sim::InlineExecutor serial; auto game = take(galactic::GalacticSession::create(config));
    submit(game, buy(0, 1)); (void)take(game.step(serial));
    submit(game, owner(1, {1}, 2, 1, 2));
    auto order = cancel(1, 1, 0); order.key.seat = {2}; submit(game, order);
    (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].empty(), "builder mismatch does not reject release cancellation");
    expect(game.snapshot()->player_economy()->players[0].credits == f(150), "refund does not credit the recorded builder");
    expect(game.snapshot()->player_economy()->players[1].credits == f(125), "refund credits the supplied recipient");
#endif // The mismatched builder intentionally violates the WGEP-64 debug assertion.
}

// WGA-159: a plan that ends while its admitted build still waits behind the front entry cancels it by identity.
void finish_task_behind_front() {
    eawr::sim::InlineExecutor serial; auto config = setup(); config.player_economy->players[0].credits = f(2000);
    config.production->players[0].pay_as_you_go = true;
    auto game = take(galactic::GalacticSession::create(config));
    submit(game, buy(0, 1)); submit(game, buy(0, 2)); submit(game, buy(0, 3)); (void)take(game.step(serial));
    const auto before = game.snapshot()->production()->planets[0].queues[0];
    expect(before.size() == 3, "three pay-as-you-go entries queue on the planet");
    const auto credits = game.snapshot()->player_economy()->players[0].credits;
    expect(static_cast<bool>(game.finish_native_production_task(before[1].build_id, false, f(1))),
        "finished task behind the front entry cancels instead of failing the session");
    const auto after = game.snapshot()->production()->planets[0].queues[0];
    expect(after.size() == 2 && after[0] == before[0] && after[1] == before[2],
        "only the finished entry leaves; the front keeps its countdown and the tail keeps its place");
    expect(game.snapshot()->player_economy()->players[0].credits == credits, "an unstarted entry has nothing paid to refund");
    expect(static_cast<bool>(game.finish_native_production_task(before[0].build_id, false, f(1))), "finished front entry still cancels");
    const auto last = game.snapshot()->production()->planets[0].queues[0];
    expect(last.size() == 1 && last[0].build_id == before[2].build_id && last[0].start_frame == game.snapshot()->clock().galactic_frame,
        "front cancellation starts its successor");
}

void queue_policies() {
    eawr::sim::InlineExecutor serial; auto config = setup(); config.player_economy->players[0].credits = f(2000);
    auto game = take(galactic::GalacticSession::create(config));
    for (std::uint64_t i = 1; i <= 6; ++i) submit(game, buy(0, i));
    const auto filled = take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].size() == 5, "human queue cap counts all entries");
    expect(filled.snapshot->events().back().value == static_cast<std::uint64_t>(galactic::ProductionAdmission::queue_full), "sixth buy reports queue full");
    submit(game, cancel(1, 7, 4)); (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].front().start_frame == 0, "tail cancellation does not restart the front");
    const auto balance = game.snapshot()->player_economy()->players[0].credits;
    submit(game, cancel(2, 8, 0, 0, galactic::ProductionAction::stop)); (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[0].queues[0].empty() && game.snapshot()->player_economy()->players[0].credits == balance, "bulk stop clears without direct refund");
    auto typed = take(galactic::GalacticSession::create(config));
    submit(typed, buy(0, 1)); submit(typed, buy(0, 2, 1, 1001)); submit(typed, buy(0, 3)); (void)take(typed.step(serial));
    const auto first = typed.snapshot()->production()->planets[0].queues[0].front().build_id;
    submit(typed, cancel(1, 4, 0, 1000, galactic::ProductionAction::cancel_type)); (void)take(typed.step(serial));
    expect(typed.snapshot()->production()->planets[0].queues[0].size() == 2 && typed.snapshot()->production()->planets[0].queues[0].front().build_id == first, "type cancel removes last matching entry");
    config.production->planets[0].offers[1].required_orbiting_units = {1000};
    auto lost = take(galactic::GalacticSession::create(config)); submit(lost, buy(0, 1, 1, 1001)); submit(lost, buy(0, 2, 1, 1001));
    (void)take(lost.step(serial)); const auto old_balance = lost.snapshot()->player_economy()->players[0].credits;
    submit(lost, orbit(1, {1}, 3, config.fleets[0].id, 2)); const auto removed = take(lost.step(serial));
    expect(lost.snapshot()->production()->planets[0].queues[0].empty() && lost.snapshot()->player_economy()->players[0].credits == old_balance, "lost prerequisites cancel without direct refund");
    std::vector<std::uint64_t> cancelled;
    for (const auto& event : removed.snapshot->events()) if (event.kind == galactic::GalacticEventKind::production_cancelled) cancelled.push_back(event.sequence);
    expect(cancelled.size() == 2 && cancelled[0] > cancelled[1], "prerequisite sweep cancels backwards");
    config = setup(); config.production->players[0].pay_as_you_go = true; config.player_economy->players[0].credits = f(0);
    auto suspended = take(galactic::GalacticSession::create(config)); submit(suspended, buy(0, 1));
    for (int i = 0; i < 3; ++i) (void)take(suspended.step(serial));
    expect(suspended.snapshot()->production()->planets[0].queues[0].front().last_remaining == config.production->planets[0].offers[0].seconds, "shortage resets timer to last paid service frontier");
}

void base_and_tech_completion() {
    eawr::sim::InlineExecutor serial; auto config = setup();
    auto& planet = config.production->planets[0]; planet.space_level = 0;
    auto station = planet.offers[0]; station.domain = galactic::ProductionDomain::space;
    station.kind = galactic::ProductionKind::starbase; station.base_level = 1; station.population = f(0);
    planet.offers[0] = station; planet.offers[1].domain = galactic::ProductionDomain::space;
    planet.objects.push_back({10000, 1001, 1, f(100), galactic::ProductionDomain::space, galactic::ProductionKind::structure});
    config.production->players[0].counts = {{1001, 1, 1}};
    auto base = take(galactic::GalacticSession::create(config));
    submit(base, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
    for (int i = 0; i < 4; ++i) (void)take(base.step(serial));
    const auto& completed = base.snapshot()->production()->planets[0];
    expect(completed.space_level == 1 && completed.starbase_type == 1000 && completed.starbase_owner == 1, "station completion records authored level/type/owner");
    expect(completed.objects.empty(), "level-one station clears orbital facilities");
    const auto& counts = base.snapshot()->production()->players[0].counts;
    expect(counts[0].owned == 1 && counts[0].historical == 1 && counts[1].owned == 0 && counts[1].historical == 1, "station creation and facility removal update owned counts without deleting history");
    config = setup(); auto& tech = config.production->planets[0].offers[0];
    tech.kind = galactic::ProductionKind::technology; tech.target_tech = 4; tech.population = f(0);
    auto& unlocked = config.production->planets[0].offers[1]; unlocked.tech = 3;
    unlocked.initially_locked = true; unlocked.slicer_unlockable = true;
    config.production->technology_types = {{1001, 3, true, true, {1}}, {2000, 0, true, true, {1}}};
    auto research = take(galactic::GalacticSession::create(config)); submit(research, buy(0, 1));
    for (int i = 0; i < 4; ++i) (void)take(research.step(serial));
    expect(research.snapshot()->player_economy()->players[0].current_tech == 4, "technology sets authored target rather than adding one");
    expect(research.snapshot()->production()->players[0].unlocked == std::vector<galactic::TypeId>{1001, 2000}, "WGEP-147 completion includes registered types absent from all offers");
}

galactic::GalacticSetup blocker_setup() {
    auto config = setup(); config.rules.fiscal_cycle_frames = 1000;
    // Both players share a human side: WGEP-24 checks different ownership, not hostility.
    config.players.push_back({2, 1, 0});
    config.seats.push_back({{2}, 2, galactic::SeatKind::human, 1, true, 2});
    config.player_economy->players.push_back({2, f(250), f(10000), 2, 5});
    config.fiscal->players.push_back({2, true, f(1)});
    config.production->players.push_back({2, true, false, true, f(50), f(0), {}, {}, {}});
    auto& station = config.production->planets[0].offers[0];
    station.domain = galactic::ProductionDomain::space;
    station.kind = galactic::ProductionKind::starbase; station.base_level = 2;
    station.population = f(0); station.seconds = take(Fixed::from_decimal("0.11"));
    config.fleets[0].owner = 2; config.fleets[0].orbit = 1;
    config.production->fleet_blockers = std::vector<galactic::FleetProductionBlocker>{{config.fleets[0].id, false, false, true}};
    return config;
}

void live_starbase_blockers() {
    eawr::sim::InlineExecutor serial;
    const auto check = [&](auto mutate, bool blocked) {
        auto config = blocker_setup(); mutate(config);
        auto game = take(galactic::GalacticSession::create(config));
        submit(game, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
        const auto result = take(game.step(serial));
        const auto& planet = game.snapshot()->production()->planets[0];
        expect(planet.queues[1].empty() == blocked, "WGEP-24 declaration and live owner/orbit admission");
        expect(game.snapshot()->player_economy()->players[0].credits == f(blocked ? 250 : 150), "blocked admission has no debit");
        if (blocked) expect(result.snapshot->events().back().value == static_cast<std::uint64_t>(galactic::ProductionAdmission::slots), "blocker uses existing slot rejection");
    };
    check([](auto&) {}, true);
    check([](auto& c) { c.fleets[0].owner = 1; }, false);
    check([](auto& c) { c.fleets[0].orbit = 2; }, false);
    check([](auto& c) { c.production->fleet_blockers->front().pending_deletion = true; }, false);
    check([](auto& c) { c.production->fleet_blockers->front().skips_space_combat = true; }, false);
    check([](auto& c) { c.production->fleet_blockers->front().cancels_starbase_production = false; }, false);
    check([](auto& c) { c.production->fleet_blockers->clear(); c.production->planets[0].starbase_blocked = true; }, false);
    check([](auto& c) { c.production->fleet_blockers.reset(); c.production->planets[0].starbase_blocked = true; }, true);
    // Admission observes departure immediately, rather than retaining a planet flag.
    auto departed = blocker_setup(); auto departure_game = take(galactic::GalacticSession::create(departed));
    submit(departure_game, orbit(0, {2}, 1, departed.fleets[0].id, 2));
    (void)take(departure_game.step(serial));
    submit(departure_game, buy(1, 1, 1, 1000, galactic::ProductionDomain::space));
    (void)take(departure_game.step(serial));
    expect(departure_game.snapshot()->production()->planets[0].queues[1].size() == 1, "departed fleet releases admission blocker");
    auto travelling = blocker_setup(); travelling.movement.emplace();
    auto& movement = *travelling.movement; movement.mode_modifier = f(1);
    movement.types = {{1000, f(1), f(100), 0, true, false, false, false}};
    movement.players = {{1, f(1), f(0), false}, {2, f(1), f(0), false}};
    for (const auto& planet : travelling.planets) movement.planets.push_back({planet.id, f(1), false});
    for (const auto& player : travelling.players) for (const auto& planet : travelling.planets)
        movement.access.push_back({player.player_id, planet.id, true, false});
    auto transit_game = take(galactic::GalacticSession::create(travelling));
    submit(transit_game, {{0, {2}, 1}, galactic::MoveFleet{travelling.fleets[0].id, {1, 2}, {}}});
    (void)take(transit_game.step(serial));
    expect(transit_game.snapshot()->fleets()[0].motion.transit.has_value(), "real departure retains an in-transit fleet");
    submit(transit_game, buy(1, 1, 1, 1000, galactic::ProductionDomain::space));
    (void)take(transit_game.step(serial));
    expect(transit_game.snapshot()->production()->planets[0].queues[1].size() == 1, "in-transit fleet does not block its retained source orbit");
    auto successor = blocker_setup(); successor.fleets[0].orbit = 2;
    successor.production->planets[0].offers[1].domain = galactic::ProductionDomain::space;
    auto promoted = take(galactic::GalacticSession::create(successor));
    submit(promoted, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
    submit(promoted, buy(0, 2, 1, 1001, galactic::ProductionDomain::space));
    (void)take(promoted.step(serial));
    submit(promoted, orbit(1, {2}, 1, successor.fleets[0].id, 1));
    (void)take(promoted.step(serial));
    const auto& front = promoted.snapshot()->production()->planets[0].queues[1].front();
    expect(front.type == 1001 && front.start_frame == 1 && front.last_remaining == front.full_seconds, "cancellation starts successor at full duration and returns before billing");
    auto invalid = blocker_setup(); invalid.production->fleet_blockers->push_back(invalid.production->fleet_blockers->front());
    expect(!galactic::validate_setup(invalid), "duplicate blocker identity rejected");
    invalid = blocker_setup(); invalid.production->fleet_blockers->front().fleet = 999999;
    expect(!galactic::validate_setup(invalid), "unknown blocker identity rejected");
    for (bool pending : {false, true}) {
        auto config = blocker_setup(); config.fleets[0].orbit = 2;
        auto game = take(galactic::GalacticSession::create(config));
        submit(game, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
        submit(game, buy(0, 2, 1, 1001, galactic::ProductionDomain::ground));
        (void)take(game.step(serial));
        // Arrive on the expiry frame: cancellation must precede starbase completion.
        submit(game, orbit(3, {2}, 1, config.fleets[0].id, 1));
        (void)take(game.step(serial)); (void)take(game.step(serial));
        const auto result = take(game.step(serial, pending));
        const auto& planet = result.snapshot->production()->planets[0];
        expect(planet.space_level == (pending ? 2 : 1), "WGEP-38 skips orbit cancellation only with pending conflict");
        expect(planet.queues[1].empty(), "expired front completed or cancelled once");
        expect(result.snapshot->player_economy()->players[0].credits == f(50), "automatic cancellation has no direct refund");
        expect(std::count_if(result.snapshot->events().begin(), result.snapshot->events().end(), [](const auto& e) {
            return e.kind == galactic::GalacticEventKind::production_cancelled;
        }) == (pending ? 0 : 1), "automatic cancellation notification emitted exactly once");
    }
}
} // namespace galactic_production_test
