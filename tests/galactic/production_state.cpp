#include "../support/env.hpp"
#include "../support/result.hpp"
#include "production_private.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/fleets.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#if defined(_MSC_VER) && !defined(NDEBUG)
#include <crtdbg.h>
#endif

namespace galactic_production_test {
using namespace galactic_test;
namespace cp = eawr::checkpoint;
namespace campaign = eawr::campaign;
namespace {
using eawr::testing::result_code_colon::take;
} // namespace

// The fixtures directory comes from CMake (argv[1]): __FILE__ is not a checkout path on every build host.
std::filesystem::path fixture_root;

void ground_structure_sale() {
    namespace gc = galactic;
    auto config = setup(); auto& planet = config.production->planets.front();
    auto& offer = planet.offers.front(); offer.kind = gc::ProductionKind::structure;
    offer.authored_price = f(101); offer.population = {};
    planet.objects = {{9001, offer.type, 1, f(101), gc::ProductionDomain::ground, gc::ProductionKind::structure},
        {9002, offer.type, 1, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure}};
    config.production->players.front().counts = {{1000, 3, 3}}; // Two structures plus the original fleet member.
    expect(gc::ground_sale_refund(planet.objects[0], offer) == f(51), "paid odd price rounds half up");
    expect(gc::ground_sale_refund(planet.objects[1], offer) == f(50), "starting object uses truncated half authored price");
    auto cheap = planet.objects[0]; cheap.paid_cost = Fixed::from_raw(Fixed::scale / 2);
    expect(gc::ground_sale_refund(cheap, offer) == f(0), "positive sub-credit payment refunds zero");
    cheap.paid_cost = f(1);
    expect(gc::ground_sale_refund(cheap, offer) == f(0), "one-credit payment refunds zero");
    expect(gc::ground_sale_admission(planet, 2, 9001) == gc::ProductionAdmission::ownership, "foreign object cannot sell");
    expect(gc::ground_sale_admission(planet, 1, 9999) == gc::ProductionAdmission::stale_entry, "sale guards exact identity");
    offer.sellable = false;
    expect(gc::ground_sale_admission(planet, 1, 9001) == gc::ProductionAdmission::unavailable, "authored never-sale gate");
    offer.sellable = true;
    const auto sale_offer = offer;
    auto queued = offer; queued.type = 2000; queued.domain = gc::ProductionDomain::space;
    queued.required_structures = {{1000}}; planet.offers.push_back(queued);
    planet.queues[1].push_back({2000, 1, 9990, f(100), f(100), f(100), 0, 3000, false});
    expect(gc::ground_sale_admission(planet, 1, 9001) == gc::ProductionAdmission::locked, "space queue also occupies ground factory");
    planet.queues[1].clear();
    eawr::sim::InlineExecutor serial;
    {
        auto blocked = take(gc::GalacticSession::create(config));
        submit(blocked, buy(0, 1, planet.planet, 2000, gc::ProductionDomain::space));
        (void)take(blocked.step(serial));
        submit(blocked, {{1, {1}, 2}, gc::SellObject{planet.planet, 9001}});
        (void)take(blocked.step(serial));
        expect(blocked.snapshot()->production()->planets.front().objects.size() == 2, "queued prerequisite prevents commanded sale");
        expect(blocked.snapshot()->events().front().value == static_cast<std::uint64_t>(gc::ProductionAdmission::locked), "busy sale has typed rejection");
    }
    const gc::CommandPayload payload = gc::SellObject{planet.planet, 9001};
    {
        auto free = config; free.rules.fiscal_cycle_frames = 1000;
        free.production->planets.front().objects.front().paid_cost = f(1);
        gc::VictoryContext context; context.players = {{1, "Synthetic", false, true, true, false, f(250)}};
        context.relations = {{1, 1, true}};
        for (const auto& world : free.planets)
            context.planets.push_back({world.id, 100, world.owner, world.owner, world.owner, false});
        gc::VictoryAuthoring authoring; authoring.human = {gc::VictoryKind::credits}; authoring.human_parameters.credits = 200;
        free.victory = gc::install_victory(authoring, context, 1);
        auto game = take(gc::GalacticSession::create(free));
        submit(game, {{0, {1}, 1}, payload}); (void)take(game.step(serial));
        expect(game.snapshot()->player_economy()->players.front().credits == f(250), "one-credit structure refunds zero");
        expect(!game.victory()->pending, "zero-refund sale does not admit an unrelated already-held credit goal");
    }
    {
        auto multiplied = config; multiplied.fiscal->players.front().positive_credit_multiplier = f(2);
        auto game = take(gc::GalacticSession::create(multiplied));
        submit(game, {{0, {1}, 1}, payload}); (void)take(game.step(serial));
        expect(game.snapshot()->player_economy()->players.front().credits == f(352), "recipient multiplier applies after the displayed 51-credit refund");
    }
    const auto bytes = gc::encode_payload(payload);
    expect(gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::sell_object, bytes) == payload,
        "sale identity payload round trip");
    auto truncated = bytes; truncated.pop_back();
    expect(!gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::sell_object, truncated), "truncated sale refuses");
    std::vector<std::string> baseline;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(gc::GalacticSession::create(config));
        submit(game, {{0, {1}, 1}, payload});
        // The second command names the removed object; it must not sell the same-type neighbour.
        submit(game, {{0, {1}, 2}, payload});
        const auto first = take(game.step(executor));
        expect(game.snapshot()->production()->planets.front().objects.size() == 1
            && game.snapshot()->production()->planets.front().objects.front().id == 9002, "sale removes one exact object");
        expect(game.snapshot()->player_economy()->players.front().credits == f(301), "sale credits the issuing player once");
        expect(first.snapshot->events().back().value == static_cast<std::uint64_t>(gc::ProductionAdmission::stale_entry), "duplicate sale rejects stale identity");
        submit(game, {{1, {1}, 3}, gc::SellObject{planet.planet, 9002}});
        const auto second = take(game.step(executor));
        expect(game.snapshot()->production()->planets.front().objects.empty(), "starting structure sells");
        expect(game.snapshot()->player_economy()->players.front().credits == f(351), "starting structure refunds authored half");
        const auto& count = game.snapshot()->production()->players.front().counts.front();
        expect(count.owned == 1 && count.historical == 3, "sale releases current count without rewriting historical production");
        const std::vector<std::string> hashes{first.state_hash.get(), second.state_hash.get()};
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "sale commands remain equal on 1/2/4/8 workers");
    }
    eawr::units::GalacticType type;
    type.build_cost_credits = f(101); type.build_time_seconds = f(1); type.named_hero = true;
    campaign::ProductionPreparation prep;
    prep.price_multiplier = prep.corruption_price_multiplier = prep.tech_purchase_multiplier = f(1);
    prep.time_multiplier = prep.base_tech_multiplier = prep.ai_time_multiplier = prep.corruption_time_multiplier = f(1);
    prep.factory_count = 1;
    expect(!take(campaign::prepare_production_offer(type, sale_offer, prep)).sellable, "default named hero sale excluded");
    type.sellable = "Sellable_Always";
    expect(take(campaign::prepare_production_offer(type, sale_offer, prep)).sellable, "authored always overrides hero default");
    type.sellable = "Sellable_Never";
    expect(!take(campaign::prepare_production_offer(type, sale_offer, prep)).sellable, "authored never forbids sale");
    type.sellable.clear(); type.named_hero = false; type.category_mask = {"Super"};
    expect(!take(campaign::prepare_production_offer(type, sale_offer, prep)).sellable, "default super category sale excluded");
}

void content_and_placement() {
    eawr::units::GalacticType source; source.build_cost_credits = take(Fixed::from_decimal("0.5"));
    source.build_time_seconds = f(6); source.tech_level = 2; source.population_value = f(1);
    auto prototype = setup().production->planets[0].offers[0];
    campaign::ProductionPreparation factors;
    expect(!campaign::prepare_production_offer(source, prototype, factors), "missing modifier providers are refused");
    factors.price_multiplier = f(1); factors.corruption_price_multiplier = f(1); factors.tech_purchase_multiplier = f(1);
    factors.time_multiplier = f(1); factors.base_tech_multiplier = f(1); factors.ai_time_multiplier = f(1);
    factors.corruption_time_multiplier = f(1); factors.factory_count = 2;
    const auto prepared = take(campaign::prepare_production_offer(source, prototype, factors));
    expect(prepared.price == f(1) && prepared.seconds == f(3), "XML price/time prepared with verified floor and factory divisor");
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto signed_source = source;
        signed_source.build_cost_credits = f(-1);
        signed_source.build_time_seconds = f(4294967295LL);
        const auto negative = take(campaign::prepare_production_offer(signed_source, prototype, factors));
        expect(negative.price == f(-1) && negative.authored_price == f(-1), "unbuyable hero sentinel reaches offer unchanged");
        expect(take(galactic::production_frames(f(4294967295LL))) == 128849018850ULL,
            "unsigned source duration fits frame arithmetic without Q24 product overflow");
        auto config = setup(); config.production->planets.front().offers.front() = negative;
        auto game = take(campaign::GameSession::create(config));
        const auto identity = cp::game_identities(config);
        const auto saved = take(cp::save_game_checkpoint(game, identity));
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto restored = take(cp::load_game_checkpoint(saved, identity, {}, executor));
        expect(take(cp::save_game_checkpoint(restored.session, identity)) == saved, "negative authored and adjusted price round trip without format changes");
        expect(bool(game.submit(buy(0, 1))), "negative price reaches purchase consumer");
        static_cast<void>(take(game.step(executor)));
        expect(game.galaxy().snapshot()->production()->planets.front().queues[0].empty(), "WGEP-18 unbuyable price cannot create production");
        auto negative_time = source;
        auto signed_factors = factors; signed_factors.time_multiplier = f(-1);
        const auto time_offer = take(campaign::prepare_production_offer(negative_time, prototype, signed_factors));
        expect(time_offer.seconds == f(-3), "negative time multiplier retained for admission");
        auto time_config = setup(); time_config.production->planets.front().offers.front() = time_offer;
        auto time_game = take(campaign::GameSession::create(time_config));
        expect(bool(time_game.submit(buy(0, 1))), "negative time reaches purchase consumer");
        static_cast<void>(take(time_game.step(executor)));
        expect(time_game.galaxy().snapshot()->production()->planets.front().queues[0].empty(), "WGEP-18 negative effective time cannot start production");
    }
    expect(galactic::production_price(take(Fixed::from_decimal("-1.5"))) == f(-2), "signed price rounding uses half away from zero");
    for (const auto field : {&campaign::ProductionPreparation::time_multiplier,
            &campaign::ProductionPreparation::base_tech_multiplier, &campaign::ProductionPreparation::ai_time_multiplier,
            &campaign::ProductionPreparation::corruption_time_multiplier}) {
        auto signed_factors = factors; signed_factors.*field = f(-1);
        expect(take(campaign::prepare_production_offer(source, prototype, signed_factors)).seconds == f(-3),
            "each signed time factor is retained, including base, AI and corruption");
    }
    auto signed_factors = factors; signed_factors.corruption_price_multiplier = f(-3);
    expect(take(campaign::prepare_production_offer(source, prototype, signed_factors)).price == f(-2),
        "negative corruption price factor uses signed half-away rounding");
    const auto maximum = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    expect(galactic::production_product(maximum, f(2)) == maximum
        && galactic::production_product(maximum, f(-2)) == Fixed::from_raw(-maximum.raw()),
        "overflowing production products saturate by sign at the fixed representation boundary");

    for (std::int64_t target = 2; target <= 5; ++target) {
        source.build_advances_tech_level = target;
        const auto technology = take(campaign::prepare_production_offer(source, prototype, factors));
        expect(technology.kind == galactic::ProductionKind::technology && technology.target_tech == target,
            "WGEP-42 offer retains each numeric target 2-5");
        auto config = setup(); config.production->planets.front().offers.front() = technology;
        auto game = take(campaign::GameSession::create(config));
        const auto identity = cp::game_identities(config);
        const auto saved = take(cp::save_game_checkpoint(game, identity));
        eawr::sim::InlineExecutor executor;
        auto restored = take(cp::load_game_checkpoint(saved, identity, {}, executor));
        expect(restored.session.galaxy().snapshot()->production()->planets.front().offers.front().target_tech == target
            && restored.session.galaxy().snapshot()->production()->planets.front().offers.front().authored_price == source.build_cost_credits
            && take(cp::save_game_checkpoint(restored.session, identity)) == saved,
            "numeric technology offer survives canonical checkpoint round trip");
    }
    source.build_advances_tech_level = -1;
    expect(!campaign::prepare_production_offer(source, prototype, factors), "negative technology offer target refuses");
    source.build_advances_tech_level.reset();
    {
        // GHUD-S57 with a mod's uncorrupted price factor 1.2 (stock FoC authors 1.0 for non-benefiting factions): the
        // prepared price carries it, the authored price does not, so the card compares against the raw cost.
        auto modded = source; modded.build_cost_credits = f(100);
        auto scaled = factors; scaled.factory_count = 1; scaled.corruption_price_multiplier = take(Fixed::from_decimal("1.2"));
        const auto offer = take(campaign::prepare_production_offer(modded, prototype, scaled));
        expect(offer.price == f(120) && offer.authored_price == f(100), "uncorrupted factor in the price, not in the authored cost");
        // A corrupted planet (0.9) replaces the uncorrupted factor: 100 x 0.9 = 90 is a discount against the raw 100.
        auto config = setup();
        galactic::EconomyCorruptionState corruption;
        galactic::CorruptionEconomyPlayer player; player.player = 1; player.faction = 1; player.playable = true;
        player.price = {take(Fixed::from_decimal("1.2")), take(Fixed::from_decimal("0.9"))}; player.time = {f(1), f(1)};
        player.tech_purchase_multiplier = f(1);
        corruption.players = {player};
        for (const auto& planet : config.planets) corruption.planets.push_back({planet.id, {}, false, false, galactic::corruption_fixture::one, {}, {}, {}});
        const auto at = [&] { return take(galactic::prepared_production_offer(offer, nullptr, config.planets, 1, 1, &corruption)); };
        expect(at().price == f(120) && at().authored_price == f(100), "uncorrupted planet keeps the raised price");
        corruption.planets[0].corruption = galactic::corruption_fixture::Type::racketeering;
        expect(at().price == f(90) && at().authored_price == f(100), "corrupted planet price is discounted against the raw cost");
    }
    auto omitted = source; omitted.build_cost_credits.reset(); omitted.build_time_seconds.reset();
    const auto zero = take(campaign::prepare_production_offer(omitted, prototype, factors));
    expect(zero.price == Fixed{} && zero.seconds == Fixed{}
        && !omitted.build_cost_credits && !omitted.build_time_seconds,
        "WGEP-92 omission defaults consume zero without changing authored facts");
    factors.tutorial = true;
    expect(take(campaign::prepare_production_offer(source, prototype, factors)).seconds == f(5), "tutorial ignores ordinary time modifiers");
    expect(take(galactic::production_frames(take(Fixed::from_decimal("0.01")))) == 0, "sub-frame duration truncates to zero");
    eawr::sim::InlineExecutor serial;
    auto config = setup(); config.production->planets[0].offers[0].domain = galactic::ProductionDomain::space;
    auto orbit_game = take(galactic::GalacticSession::create(config));
    submit(orbit_game, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
    for (int i = 0; i < 4; ++i) (void)take(orbit_game.step(serial));
    expect(orbit_game.snapshot()->fleets().size() == config.fleets.size() + 1, "completed ship uses fleet creation route");
    expect(orbit_game.snapshot()->fleet_control()->detached_units.empty(), "produced identity attached once");
    expect(orbit_game.snapshot()->production()->planets[0].objects[0].paid_cost == f(100), "orbiting result retains paid cost");
    config = setup(); config.production->players[0].pay_as_you_go = true;
    auto gradual = take(galactic::GalacticSession::create(config)); submit(gradual, buy(0, 1));
    (void)take(gradual.step(serial)); expect(gradual.snapshot()->player_economy()->players[0].credits == f(250), "pay-as-you-go does not precharge");
    (void)take(gradual.step(serial));
    const auto charged = gradual.snapshot()->player_economy()->players[0].credits;
    expect(charged < f(250) && charged > f(200), "unfinished service pays a fractional bill");
    submit(gradual, cancel(2, 2, 0)); (void)take(gradual.step(serial));
    // Elapsed/full versus 1-remaining/full can differ by one Q24 ratio unit,
    // multiplied by the stored 100-credit price; bound both directions.
    const auto restored = gradual.snapshot()->player_economy()->players[0].credits.raw();
    expect(restored >= f(250).raw() - 100 && restored <= f(250).raw() + 100, "cancel refunds stored serviced fraction");
}

void shared_technology_writer() {
    auto config = setup(); config.rules.fiscal_cycle_frames = 1000;
    config.production->players[0].locked = {2000};
    config.production->technology_types = {{1001, 2, true, true, {1}}, {2000, 0, true, true, {1}},
        {2001, 4, true, true, {1}}, {2002, 0, true, true, {}}, {2003, 0, false, true, {1}},
        {2004, 0, true, false, {1}}};
    auto account = config.player_economy->players[0]; auto player = config.production->players[0];
    auto assigned = take(galactic::assign_technology(account, player, config.production->technology_types, 2, true,
        [&](auto id, auto tech) {
            expect(id == 1 && tech == account.current_tech && player.unlocked.empty(),
                "WGEP-130 same-level assignment notifies after scalar write and before enumeration");
            return eawr::core::Result<void>::success();
        }));
    expect(assigned == 1 && player.unlocked == std::vector<galactic::TypeId>{2000} && player.locked == std::vector<galactic::TypeId>{2000},
        "WGEP-147 strict boundary, affiliations and independent explicit locks");
    std::string baseline;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(campaign::GameSession::create(config)); auto& galaxy = game.galaxy();
        std::vector<std::int64_t> notices;
        game.set_technology_notice_handler([&](const auto& notice) {
            expect(notice.kind == galactic::GalacticSession::TechnologyNotice::Kind::assigned,
                "no local observer means no presentation sound request");
            notices.push_back(notice.tech); return eawr::core::Result<void>::success();
        });
        const auto old = galaxy.snapshot();
        expect(take(game.set_technology({1, 2, true, true})) == 2, "direct same-level writer stays active");
        expect(old->production()->players[0].unlocked.empty(), "old publication remains immutable");
        expect(take(game.set_technology({1, 99, false, false})) == 5, "target clamps without changing maximum");
        expect(galaxy.snapshot()->production()->players[0].unlocked == std::vector<galactic::TypeId>{2000}, "explicit unlock flag respected");
        const auto before = galaxy.native_effects().size();
        expect(take(game.increment_technology(1)) == 5 && galaxy.native_effects().size() == before && notices.size() == 2,
            "WGEP-148 maximum increment has no setter/story/unlock pass");
        expect(take(game.set_technology({1, 5, true, true})) == 5 && notices.size() == 3,
            "same-level direct setter differs from maximum increment");
        expect(galaxy.snapshot()->production()->players[0].unlocked == std::vector<galactic::TypeId>{1001, 2000, 2001},
            "bulk unlock covers the whole registry, never just producer offers");
        expect(galaxy.snapshot()->production()->planets[0].space_level == 1, "global technology never changes station level");
        (void)take(game.step(pool));
        const auto identities = cp::game_identities(config);
        const auto saved = take(cp::save_game_checkpoint(game, identities));
        auto restored = take(cp::load_game_checkpoint(saved, identities, {}, pool));
        expect(take(cp::save_game_checkpoint(restored.session, identities)) == saved,
            "writer journal, maximum and complete registry survive canonical reconstruction");
        expect(take(restored.session.increment_technology(1)) == 5, "first restored increment still skips setter at maximum");
        (void)take(game.step(pool)); (void)take(restored.session.step(pool));
        expect(game.galaxy().snapshot()->sha256() == restored.session.galaxy().snapshot()->sha256(), "continued tech readers agree after load");
        const auto hash = galaxy.snapshot()->sha256();
        if (baseline.empty()) baseline = hash;
        else expect(hash == baseline, "shared technology writer equal on 1/2/4/8 workers");
    }
}

// #683, WST-41/64: the story writers of the production lock lists and the technology maximum, journaled and replayed.
void story_production_writers() {
    auto config = setup(); config.rules.fiscal_cycle_frames = 1000;
    config.production->players[0].locked = {2000};
    config.production->technology_types = {{1001, 2, true, true, {1}}, {2000, 0, true, true, {1}},
        {2001, 4, true, true, {1}}, {2002, 0, true, true, {}}, {2003, 0, false, true, {1}}, {2004, 0, true, false, {1}}};
    using Ids = std::vector<galactic::TypeId>;
    std::string baseline;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(campaign::GameSession::create(config)); const auto& galaxy = game.galaxy();
        const auto player = [&]() -> const galactic::ProductionPlayer& { return galaxy.snapshot()->production()->players[0]; };
        const auto account = [&]() -> const galactic::GalacticPlayerEconomy& { return galaxy.snapshot()->player_economy()->players[0]; };
        // WST-64: the unlock first (the build lock goes, the type is listed), then the lock.
        (void)take(game.apply_production_lock({1, 2000, 2001, false}));
        expect(player().locked == Ids{2001} && player().unlocked == Ids{2000}, "WST-64 unlock removes its lock and lists the type, then the lock is listed");
        (void)take(game.apply_production_lock({1, 2003, 2003, false}));
        expect(player().locked == Ids{2001, 2003} && player().unlocked == Ids{2000, 2003}, "a type unlocked and locked by one write is on both lists");
        const auto before = galaxy.native_effects().size();
        expect(!game.apply_production_lock({1, 9999, 0, false}) && galaxy.native_effects().size() == before,
            "an unregistered type is refused before anything is journaled");
        // The faction post-check: with nothing left to unlock at or below the level, the level goes up by one.
        expect(take(game.set_technology({1, 1, false, false})) == 1, "level 1 for the post-check");
        expect(take(game.apply_production_lock({1, 0, 0, true})).tech_incremented && account().current_tech == 2,
            "WST-64 post-check with no slicer unlock left raises the level, also when both types are absent");
        expect(!take(game.apply_production_lock({1, 0, 0, true})).tech_incremented && account().current_tech == 2,
            "an unlockable type at the level (1001) keeps the level");
        expect(take(game.apply_production_lock({1, 1001, 0, true})).tech_incremented && account().current_tech == 3,
            "unlocking it leaves nothing at or below the level, so the level rises");
        expect(!take(game.apply_production_lock({1, 0, 0, false})).tech_incremented && account().current_tech == 3, "no post-check without the faction flag");
        // WST-41: SET_TECH_LEVEL raises the maximum when needed, SET_MAX_TECH_LEVEL moves the maximum alone.
        using Maximum = galactic::GalacticSession::TechnologyMaximum;
        expect(take(game.set_technology({1, 7, true, true, Maximum::raise_to_target})) == 7 && account().maximum_tech == 7, "the maximum rises to the target");
        expect(take(game.set_technology({1, 9, false, false, Maximum::set_only})) == 7 && account().maximum_tech == 9 && account().current_tech == 7,
            "a maximum write changes nothing else");
        const auto kept = galaxy.native_effects().size();
        expect(!game.set_technology({1, 3, false, false, Maximum::set_only}) && galaxy.native_effects().size() == kept,
            "a maximum below the current level is refused before the journal");
        expect(take(game.set_technology({1, 4, true, true, Maximum::raise_to_target})) == 4 && account().maximum_tech == 9, "a lower target keeps the maximum");
        (void)take(game.step(pool));
        // Every write is a native effect: the recording alone reconstructs the same world.
        const auto replay = game.record();
        expect(std::count_if(replay.native_effects.begin(), replay.native_effects.end(),
            [](const auto& e) { return e.kind == galactic::NativeEffectKind::production_lock; }) == 6, "six production lock writes are journaled");
        auto copy = take(campaign::GameSession::from_replay(replay));
        while (copy.completed_tick() < replay.final_tick_count) (void)take(copy.step(pool));
        expect(copy.galaxy().snapshot()->sha256() == galaxy.snapshot()->sha256(), "the journal reconstructs the lock lists and the maximum");
        const auto hash = galaxy.snapshot()->sha256();
        if (baseline.empty()) baseline = hash;
        else expect(hash == baseline, "story production writers equal on 1/2/4/8 workers");
    }
}

void production_corpus_bytes() {
    // Current production payloads retain an absent optional blocker declaration table.
    const auto directory = fixture_root / "production-corpus";
    eawr::sim::InlineExecutor serial;
    for (std::string_view name : {"queues-basic", "queues-orbit", "queues-paygo"}) {
        std::ifstream file(directory / (std::string(name) + ".eawr-gcreplay"), std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (bytes.empty()) throw std::runtime_error("missing production corpus recording " + (directory / (std::string(name) + ".eawr-gcreplay")).string());
        const auto replay = take(galactic::parse_replay(bytes));
        expect(!replay.setup.production->fleet_blockers, "production retains absent declaration table");
        expect(take(galactic::write_replay(replay)) == bytes, "current production replay bytes round trip");
        std::ifstream pins(directory / (std::string(name) + ".hashes.csv")); std::string line;
        std::getline(pins, line); auto game = take(campaign::GameSession::from_replay(replay));
        while (std::getline(pins, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto hash = line.substr(line.find(',') + 1);
            expect(take(game.step(serial)).state_hash.get() == hash, "current production state pins match");
        }
        expect(game.completed_tick() == replay.final_tick_count, "all current production pins checked");
    }
    auto config = blocker_setup(); config.production->fleet_blockers->clear();
    galactic::GalacticReplay empty{config, 0, {}};
    expect(take(galactic::parse_replay(take(galactic::write_replay(empty)))) == empty, "present-empty live declaration table round trip");
}

void blocker_parity_checkpoint() {
    auto config = blocker_setup(); config.fleets[0].orbit = 2;
    galactic::GalacticReplay replay{config, 8, {buy(0, 1, 1, 1000, galactic::ProductionDomain::space),
        orbit(2, {2}, 1, config.fleets[0].id, 1), orbit(3, {2}, 2, config.fleets[0].id, 2),
        buy(4, 2, 1, 1000, galactic::ProductionDomain::space)}};
    const auto bytes = take(galactic::write_replay(replay));
    expect(take(galactic::parse_replay(bytes)) == replay, "blocker declaration replay round trip");
    for (std::size_t i = 0; i < bytes.size(); i += 29)
        expect(!galactic::parse_replay(std::span(bytes).first(i)), "blocker replay truncation rejected");
    eawr::sim::InlineExecutor serial; auto baseline = take(campaign::GameSession::from_replay(replay));
    std::vector<std::string> hashes;
    for (std::uint64_t i = 0; i < replay.final_tick_count; ++i) hashes.push_back(take(baseline.step(serial)).state_hash.get());
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        for (std::uint64_t barrier : {1U, 3U, 5U}) {
            auto game = take(campaign::GameSession::from_replay(replay));
            while (game.completed_tick() < barrier) (void)take(game.step(pool));
            const auto identity = cp::game_identities(replay.setup);
            const auto saved = take(cp::save_game_checkpoint(game, identity));
            auto loaded = take(cp::load_game_checkpoint(saved, identity, {}, pool));
            expect(take(cp::save_game_checkpoint(loaded.session, identity)) == saved, "blocker checkpoint canonical round trip");
            expect(loaded.session.galaxy().snapshot()->production()->fleet_blockers == config.production->fleet_blockers, "checkpoint retains blocker declarations");
            while (loaded.session.completed_tick() < replay.final_tick_count) {
                const auto tick = loaded.session.completed_tick(); loaded.session.scramble_storage_for_testing();
                expect(take(loaded.session.step(pool)).state_hash.get() == hashes[tick], "blocker arrival/departure 1/2/4/8 equality after restore");
            }
        }
    }
}

galactic::GalacticReplay corpus() {
    auto config = setup(); config.player_economy->players[0].credits = f(2000);
    std::vector<galactic::GameCommand> orders{buy(0, 1), buy(0, 2), buy(0, 3, 2, 1001),
        cancel(1, 4, 1), buy(2, 5), {{3, {1}, 6}, galactic::PauseGalaxy{}},
        {{6, {1}, 7}, galactic::ResumeGalaxy{}}, cancel(7, 8, 0, 1000, galactic::ProductionAction::cancel_type), buy(8, 9)};
    return {config, 16, orders};
}

galactic::GalacticSetup census_setup() {
    auto config = setup(); config.player_economy->players[0].credits = f(2000);
    config.census = galactic::PopulationCensus{{{1000, f(1), 0}, {1001, f(0), 0}}, {{1, 2, false, {}}}, {}};
    return config;
}

void native_action_production_barrier() {
    namespace gc = galactic;
    const auto require = [](const auto& result) { if (!result) throw std::runtime_error(result.error().message); };
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto config = census_setup(); config.rules.fiscal_cycle_frames = 1000;
        config.production->players.front().human = false; config.production->players.front().pay_as_you_go = true;
        config.seats.front().kind = gc::SeatKind::ai;
        config.seats.front().human_side_id = 0;
        config.census->players.front().base_capacity = 20; config.census->players.front().ai_uses_cap = true;
        config.fleet_control->types = {{1000, false, true, false}, {1001, false, true, false}};
        config.movement.emplace(); auto& movement = *config.movement; movement.mode_modifier = f(1);
        movement.types = {{1000, f(1), f(100), 0, true, false, false, false}, {1001, f(1), f(100), 0, true, false, false, false}};
        movement.players = {{1, f(1), f(1), false}};
        for (const auto& planet : config.planets) {
            movement.planets.push_back({planet.id, f(1), false}); movement.access.push_back({1, planet.id, true, false});
        }
        config.control.emplace(); auto& control = *config.control; control.change_seconds = f(7); control.max_ground_forces = 10;
        control.players = {{1, true, false, config.players.front().faction_id}}; control.relations = {{1, 1, true}}; control.types = {{1000, 1, false, false}, {1001, 1, false, false}};
        for (const auto& planet : config.planets) {
            gc::ControlPlanet row; row.planet = planet.id; row.type = planet.id; row.surface_accessible = true;
            row.ground_base = {1, 1}; row.starbase = {1, 1}; control.planets.push_back(row);
        }
        for (const auto& route : config.routes) control.routes.push_back({route.id, f(1)});
        auto& creation = config.production->nested_creation.emplace(); creation.fleet_type = 1001;
        creation.types = {{1000, 0, {}, {}, false, false, gc::ActorCombatRole::ordinary, {}},
            {1001, 0, {}, {}, false, false, gc::ActorCombatRole::ordinary, {}}}; creation.relations = {{1, 1, false}};
        for (const auto& fleet : config.fleets) for (const auto& member : fleet.members) {
            gc::CensusObject actor; actor.id = member.id; actor.original_type = actor.current_type = member.type;
            actor.owner = fleet.owner; actor.planet = fleet.orbit; actor.container = fleet.id; actor.combat_role = gc::ActorCombatRole::ordinary;
            config.census->objects.push_back(actor);
        }
        std::sort(config.census->objects.begin(), config.census->objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        config.income.emplace(); gc::EconomicType mine; mine.type = 1000; mine.mining = gc::PlanetIncomeAbility{{}, f(7), f(1)};
        config.income->types.push_back(mine);
        for (auto& planet : config.production->planets) {
            config.income->planets.push_back({planet.planet, false, false, false});
            for (auto& offer : planet.offers) {
                offer.seconds = f(10); config.income->offers.push_back({planet.planet, offer.type, offer.price, offer.seconds});
            }
        }
        config.production->fleet_blockers.emplace();
        for (const auto& fleet : config.fleets) config.production->fleet_blockers->push_back({fleet.id, false, false, true});
        auto staged = take(gc::GalacticSession::create(config)); auto direct = take(gc::GalacticSession::create(config));
        std::vector<std::string> staged_notices, direct_notices;
        const auto bind = [&](gc::GalacticSession& world, std::vector<std::string>& notices) {
            auto* owner = &world; auto* output = &notices;
            const auto publication = [owner] { return eawr::core::sha256_hex(owner->snapshot()->canonical_bytes()); };
            world.set_native_production_handler([output, publication](const auto& input) {
                output->push_back("mark:" + std::to_string(static_cast<unsigned>(input.kind)) + ":" + std::to_string(input.build)
                    + ":" + std::to_string(input.amount.raw()) + ":" + publication());
                return eawr::core::Result<gc::GalacticSession::NativeProductionPayment>::success({true, input.player, input.amount});
            });
            world.set_credit_notification_handler([output, publication](const auto& input) {
                output->push_back("cash:" + std::to_string(input.player) + ":" + std::to_string(input.credits.raw())
                    + ":" + std::to_string(input.amount.raw()) + ":" + publication());
                return eawr::core::Result<void>::success();
            });
            world.set_native_production_outcome_handler([output, publication](const auto& input) {
                output->push_back("outcome:" + std::to_string(input.build) + ":" + std::to_string(input.completed) + ":" + publication());
            });
            world.set_native_actor_changes_handler([output](const auto& snapshot, auto changes) {
                auto value = "actors:" + eawr::core::sha256_hex(snapshot.canonical_bytes());
                for (const auto& change : changes) value += ":" + std::to_string(static_cast<unsigned>(change.kind)) + "/"
                    + std::to_string(change.actor) + "/" + std::to_string(change.source);
                output->push_back(std::move(value)); return eawr::core::Result<void>::success();
            });
        };
        bind(staged, staged_notices); bind(direct, direct_notices);
        const auto planet = config.planets.front().id;
        const auto placement = [planet](eawr::sim::EntityId frontier, const std::vector<Fixed>& special) {
            gc::GalacticSession::NativeForcePlacement output; output.next_id = frontier + 1; output.special_control = special;
            output.structures.push_back({frontier, 1000, 1, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure});
            gc::CensusObject actor; actor.id = frontier; actor.original_type = actor.current_type = 1000;
            actor.owner = 1; actor.planet = actor.container = planet; actor.combat_role = gc::ActorCombatRole::ordinary;
            output.originals.push_back(actor); return output;
        };
        const auto full_builder = [&](const gc::GalacticSnapshot& snapshot, eawr::sim::EntityId frontier) {
            return eawr::core::Result<gc::GalacticSession::NativeForcePlacement>::success(placement(frontier, snapshot.control()->planets.front().special_control));
        };
        const auto private_builder = [&](const gc::GalacticSession::NativeForceView& view, eawr::sim::EntityId frontier) {
            return eawr::core::Result<gc::GalacticSession::NativeForcePlacement>::success(placement(frontier, view.presence.special_control));
        };
        const std::array<gc::PlayerId, 1> players{1};
        const auto check = [&] {
            expect(staged.snapshot()->canonical_bytes() == direct.snapshot()->canonical_bytes(), "native producer publication matches the live oracle");
            expect(staged.snapshot()->economy_input_generation()->income == direct.snapshot()->economy_input_generation()->income,
                "native producer income projection matches the live source gather");
            expect(staged_notices == direct_notices, "native funding, outcome and actor notices retain their order and publication boundary");
        };
        const auto commit_barrier = [&](auto& pending, bool publishes = true) {
            const auto profiling = eawr::gc_profile::enabled.exchange(true);
            const auto index = static_cast<std::size_t>(eawr::gc_profile::Phase::make_snapshot);
            const auto before_calls = eawr::gc_profile::snapshot()[index].calls;
            auto committed = pending->commit();
            const auto calls = eawr::gc_profile::snapshot()[index].calls - before_calls;
            eawr::gc_profile::enabled.store(profiling);
            require(committed);
            expect(calls == (publishes ? 1U : 0U), "a prepared native barrier constructs at most one full snapshot");
        };
        auto initial = staged.canonical_state_bytes();
        auto opened = staged.begin_native_barrier(players, 64); require(opened); auto barrier = std::move(opened).value(); require(barrier->select_player(1));
#ifndef NDEBUG
#ifdef _MSC_VER
        char probe_value[2]{}; std::size_t probe_length{};
        const bool foreign_probe = getenv_s(&probe_length, probe_value, sizeof(probe_value), "EAWR_GC_FOREIGN_BARRIER_PROBE") == 0
            && std::string_view(probe_value) == "1";
#else
        const bool foreign_probe = std::getenv("EAWR_GC_FOREIGN_BARRIER_PROBE") != nullptr;
#endif
        if (foreign_probe) {
#ifdef _MSC_VER
            _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
            _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
            std::thread foreign([&] { (void)staged.snapshot(); });
            foreign.join();
            throw std::runtime_error("foreign-thread barrier probe did not assert");
        }
#endif
        const auto initial_income = staged.snapshot()->economy_input_generation()->income->front().second;
        auto made = staged.create_native_force({1, planet, 1000, {}}, full_builder);
        auto oracle_made = direct.create_native_force({1, planet, 1000, {}}, full_builder);
        require(made); require(oracle_made); expect(made.value() == oracle_made.value(), "staged magic reserves the live identity frontier"); check();
        expect(staged.snapshot()->economy_input_generation()->income->front().second == take(eawr::sim::math::add(initial_income, f(7))),
            "a new mining structure contributes exactly once before the next player read");
        const auto published = staged.snapshot(); const auto oracle_published = direct.snapshot();
        auto refused = staged.admit_native_production({1, planet, 999999, f(100)}, executor);
        auto oracle_refused = direct.admit_native_production({1, planet, 999999, f(100)}, executor);
        require(refused); require(oracle_refused); expect(refused.value().admission == oracle_refused.value().admission && !refused.value().build,
            "refused native offer refreshes private census without inventing a marker");
        expect(staged.snapshot() == published && direct.snapshot() == oracle_published, "refused native offer does not publish its private count refresh"); check();
        auto cash = staged.add_credits(1, f(13), f(1)); auto oracle_cash = direct.add_credits(1, f(13), f(1));
        require(cash); require(oracle_cash); expect(cash.value() == oracle_cash.value(), "staged cash returns the exact capped windfall"); check();
        auto admitted = staged.admit_native_production({1, planet, 1000, f(100)}, executor);
        auto oracle_admitted = direct.admit_native_production({1, planet, 1000, f(100)}, executor);
        require(admitted); require(oracle_admitted);
        expect(admitted.value().admission == gc::ProductionAdmission::allowed && admitted.value().build == oracle_admitted.value().build
            && admitted.value().price == oracle_admitted.value().price, "staged paid queue uses the current producer and population admission"); check();
        const auto build = admitted.value().build;
        expect(staged.canonical_state_bytes() == initial, "producer decisions leave the main galaxy unchanged");
        require(barrier->begin_tail()); commit_barrier(barrier); barrier.reset();
        expect(staged.canonical_state_bytes() == direct.canonical_state_bytes(), "first producer barrier commits the exact live state"); check();
        std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 2; ++tick) {
            auto actual = staged.step(executor); auto oracle = direct.step(executor); require(actual); require(oracle);
            expect(actual.value().state_sha256 == oracle.value().state_sha256, "paid queue progress matches after staged admission"); hashes.push_back(actual.value().state_sha256);
        }
        initial = staged.canonical_state_bytes(); opened = staged.begin_native_barrier(players, 64); require(opened); barrier = std::move(opened).value();
        const auto running_front = staged.snapshot()->production()->planets.front().queues[0].front();
        auto waiting = staged.admit_native_production({1, planet, 1000, f(100)}, executor);
        auto oracle_waiting = direct.admit_native_production({1, planet, 1000, f(100)}, executor);
        require(waiting); require(oracle_waiting);
        expect(waiting.value().admission == gc::ProductionAdmission::allowed && waiting.value().build == oracle_waiting.value().build,
            "a second native paid task waits behind the running front"); check();
        require(staged.finish_native_production_task(waiting.value().build, false, f(1)));
        require(direct.finish_native_production_task(oracle_waiting.value().build, false, f(1))); check();
        expect(staged.snapshot()->production()->planets.front().queues[0].front() == running_front,
            "staged cancellation behind the front preserves its running countdown");
        require(staged.finish_native_production_task(build, false, f(1))); require(direct.finish_native_production_task(build, false, f(1))); check();
        require(barrier->begin_players()); require(barrier->select_player(1));
        auto batch_result = staged.begin_native_actions(16); auto oracle_batch_result = direct.begin_native_actions(16);
        require(batch_result); require(oracle_batch_result); auto batch = std::move(batch_result).value(); auto oracle_batch = std::move(oracle_batch_result).value();
        cash = batch.add_credits(1, f(7), f(1)); oracle_cash = oracle_batch.add_credits(1, f(7), f(1)); require(cash); require(oracle_cash);
        made = batch.create_force({1, planet, 1000, {}}, private_builder, executor);
        oracle_made = oracle_batch.create_force({1, planet, 1000, {}}, private_builder, executor);
        require(made); require(oracle_made); expect(made.value() == oracle_made.value(), "private immediate creation reserves matching identities");
        cash = batch.add_credits(1, f(-2), f(1)); oracle_cash = oracle_batch.add_credits(1, f(-2), f(1)); require(cash); require(oracle_cash);
        require(staged.commit_native_actions(batch)); require(direct.commit_native_actions(oracle_batch)); check();
        require(barrier->begin_tail());
        cash = staged.add_credits(1, f(3), f(1)); oracle_cash = direct.add_credits(1, f(3), f(1)); require(cash); require(oracle_cash); check();
        const auto last_publication = staged.snapshot(); const auto oracle_last = direct.snapshot();
        refused = staged.admit_native_production({1, planet, 999999, f(100)}, executor);
        oracle_refused = direct.admit_native_production({1, planet, 999999, f(100)}, executor); require(refused); require(oracle_refused);
        expect(staged.snapshot() == last_publication && direct.snapshot() == oracle_last, "last refused request keeps the prior publication at barrier end"); check();
        expect(staged.canonical_state_bytes() == initial, "prelude, immediate batch and tail remain private until ordered commit");
        commit_barrier(barrier); barrier.reset();
        expect(staged.canonical_state_bytes() == direct.canonical_state_bytes() && staged.record() == direct.record(),
            "producer prelude, immediate grouping and tail commit exact state and journal bytes"); check();
        opened = staged.begin_native_barrier(players, 16); require(opened); barrier = std::move(opened).value();
        require(barrier->begin_players()); require(barrier->select_player(1));
        batch_result = staged.begin_native_actions(1); oracle_batch_result = direct.begin_native_actions(1);
        require(batch_result); require(oracle_batch_result);
        batch = std::move(batch_result).value(); oracle_batch = std::move(oracle_batch_result).value();
        require(staged.commit_native_actions(batch)); require(direct.commit_native_actions(oracle_batch)); check();
        require(barrier->begin_tail()); commit_barrier(barrier); barrier.reset();
        expect(staged.canonical_state_bytes() == direct.canonical_state_bytes(),
            "an empty immediate batch publishes a prior refused request's private census exactly once"); check();
        opened = staged.begin_native_barrier(players, 16); require(opened); barrier = std::move(opened).value();
        require(barrier->begin_players()); require(barrier->select_player(1));
        const auto created_fleet = staged.next_entity_id();
        const auto space_builder = [planet](const gc::GalacticSnapshot&, eawr::sim::EntityId frontier) {
            gc::GalacticSession::NativeForcePlacement output; output.next_id = frontier + 3;
            output.fleets.push_back({frontier, 1, planet, {{frontier + 1, 1001}, {frontier + 2, 1001}}});
            for (eawr::sim::EntityId offset = 0; offset < 3; ++offset) {
                gc::CensusObject actor; actor.id = frontier + offset; actor.original_type = actor.current_type = 1001;
                actor.owner = 1; actor.planet = planet; actor.container = offset ? frontier : planet;
                actor.combat_role = offset ? gc::ActorCombatRole::ordinary : gc::ActorCombatRole::fleet;
                if (offset == 1) { actor.abilities.present = true; actor.abilities.combat_skip = true; actor.abilities.self_target = true; }
                output.originals.push_back(actor);
            }
            return eawr::core::Result<gc::GalacticSession::NativeForcePlacement>::success(std::move(output));
        };
        made = staged.create_native_force({1, planet, 1001, {}}, space_builder);
        oracle_made = direct.create_native_force({1, planet, 1001, {}}, space_builder);
        require(made); require(oracle_made); expect(made.value() == oracle_made.value(), "new fleet constructor identities match"); check();
        auto detached = staged.admit_native_fleet(1, gc::RemoveFleetMembers{created_fleet, {created_fleet + 2}}, false);
        auto oracle_detached = direct.admit_native_fleet(1, gc::RemoveFleetMembers{created_fleet, {created_fleet + 2}}, false);
        require(detached); require(oracle_detached); expect(detached.value().admitted && oracle_detached.value().admitted, "new fleet membership changes are admitted"); check();
        const auto& blockers = *staged.snapshot()->production()->fleet_blockers;
        const auto blocker = std::find_if(blockers.begin(), blockers.end(), [created_fleet](const auto& row) { return row.fleet == created_fleet; });
        expect(blocker != blockers.end() && blocker->skips_space_combat,
            "a fleet created in an earlier layer refreshes its live combat-skip blocker after membership changes");
        const auto built_skip = staged.snapshot()->fleet_combat_skip(created_fleet);
        expect(bool(built_skip) && built_skip.value(), "GST-U01: a freshly built stealth-only fleet skips combat");
        require(barrier->begin_tail()); commit_barrier(barrier); barrier.reset();
        expect(staged.canonical_state_bytes() == direct.canonical_state_bytes(), "new fleet blocker commits the exact live state"); check();
        const auto unchanged = staged.snapshot();
        opened = staged.begin_native_barrier(players, 16); require(opened); barrier = std::move(opened).value();
        require(barrier->begin_players()); require(barrier->begin_tail()); commit_barrier(barrier, false); barrier.reset();
        expect(staged.snapshot() == unchanged, "an untouched barrier retains its completed publication without a full rebuild");
        opened = staged.begin_native_barrier(players, 256); require(opened); barrier = std::move(opened).value();
        require(barrier->select_player(1));
        auto previous_generation = staged.snapshot()->economy_input_generation()->accounts;
        for (unsigned action = 0; action < 64; ++action) {
            const auto delta = f(action % 2 ? -1 : 1);
            require(staged.add_credits(1, delta, f(1))); require(direct.add_credits(1, delta, f(1)));
            const auto generation = staged.snapshot()->economy_input_generation()->accounts;
            expect(generation > previous_generation, "coalesced account publications advance their derived invalidation key");
            previous_generation = generation;
        }
        require(staged.add_credits(1, f(5), f(1))); require(direct.add_credits(1, f(5), f(1)));
        auto shifted = staged.admit_native_fleet(1, gc::SetFleetOrbitSlot{created_fleet, 2}, false);
        auto oracle_shifted = direct.admit_native_fleet(1, gc::SetFleetOrbitSlot{created_fleet, 2}, false);
        require(shifted); require(oracle_shifted); expect(shifted.value().admitted && oracle_shifted.value().admitted,
            "checkpoint fixture changes a fleet in the current player frontier");
        made = staged.create_native_force({1, planet, 1000, {}}, full_builder);
        oracle_made = direct.create_native_force({1, planet, 1000, {}}, full_builder);
        require(made); require(oracle_made);
        const auto checkpoint_actor = made.value().front();
        expect(barrier->work().layers == 2, "ordinary actions retain only the prelude and current player layers"); check();
        for (unsigned transaction = 0; transaction < 40; ++transaction) {
            auto empty = staged.begin_native_actions(1); auto oracle_empty = direct.begin_native_actions(1);
            require(empty); require(oracle_empty);
            require(staged.commit_native_actions(empty.value())); require(direct.commit_native_actions(oracle_empty.value()));
        }
        expect(barrier->work().read_checkpoints > 0, "long immediate chains checkpoint their sparse read frontier");
        {
            auto probe = staged.begin_native_actions(1); require(probe);
            const auto* inherited_fleet = probe.value().fleet(created_fleet);
            expect(inherited_fleet && inherited_fleet->orbit_slot == 2 && inherited_fleet->members.size() == 1
                && inherited_fleet->members.front().id == created_fleet + 1,
                "checkpoint reads retain the newest sparse fleet membership");
            expect(probe.value().actor(checkpoint_actor) && probe.value().created_object(checkpoint_actor)
                && probe.value().credits(1) == direct.snapshot()->player_economy()->players.front().credits,
                "checkpoint reads retain actor and account frontiers");
        }
        require(barrier->begin_tail()); commit_barrier(barrier); barrier.reset();
        expect(staged.canonical_state_bytes() == direct.canonical_state_bytes() && staged.record() == direct.record(),
            "coalesced actions and checkpointed immediate batches preserve state and journal bytes"); check();
        {
            auto aborted_result = gc::GalacticSession::create(config); require(aborted_result);
            auto aborted = std::move(aborted_result).value();
            const auto completed_publication = aborted.snapshot();
            auto pending_result = aborted.begin_native_barrier(players, 16); require(pending_result);
            std::thread destroy([pending = std::move(pending_result).value()]() mutable { pending.reset(); });
            destroy.join(); // the creator is quiescent for the entire foreign destruction
            expect(aborted.snapshot() == completed_publication
                && !aborted.begin_native_barrier(players, 16) && !aborted.step(executor),
                "foreign barrier destruction expires the creator context and leaves an aborted owner");
        }
        {
            auto bounded_result = gc::GalacticSession::create(config); require(bounded_result);
            auto bounded = std::move(bounded_result).value();
            auto pending_result = bounded.begin_native_barrier(players, 1); require(pending_result);
            auto pending = std::move(pending_result).value();
            auto first_empty = bounded.begin_native_actions(1); require(first_empty);
            require(bounded.commit_native_actions(first_empty.value()));
            auto second_empty = bounded.begin_native_actions(1); require(second_empty);
            expect(!bounded.commit_native_actions(second_empty.value()), "empty transactions cannot bypass the barrier retention budget");
            expect(!pending->commit(), "a rejected retention boundary cannot later enter ordered replay");
            pending.reset();
            expect(!bounded.step(executor), "an exhausted native barrier is unusable after abandonment");
        }
        for (unsigned tick = 0; tick < 12; ++tick) {
            auto actual = staged.step(executor); auto oracle = direct.step(executor); require(actual); require(oracle);
            expect(actual.value().state_sha256 == oracle.value().state_sha256, "producer continuation matches after cancellation and grouped creation"); hashes.push_back(actual.value().state_sha256);
        }
        if (workers == 1) reference = hashes; else expect(reference == hashes, "producer barrier continuation is equal at 1/2/4/8 workers");
    }
}

void unit_sale() {
    namespace gc = galactic;
    const auto config_for = [&](bool landed) {
        auto config = census_setup(); config.rules.fiscal_cycle_frames = 1000;
        config.census->players.front().base_capacity = 20;
        config.fleet_control->types.front().transport = true;
        config.census->types.push_back({1002, f(3), 0, {1000, 1001}, true, 1000});
        config.census->types.back().authored_price = f(101);
        config.movement.emplace(); auto& movement = *config.movement;
        movement.mode_modifier = f(1);
        movement.types = {{1000, f(3), {}, 0, true, false, false, false, true},
            {1001, {}, {}, 0, false, false, false, false}};
        movement.players = {{1, f(1), {}, false}};
        for (const auto& planet : config.planets) {
            movement.planets.push_back({planet.id, {}, false}); movement.access.push_back({1, planet.id, true, false});
        }
        const auto fleet = config.fleets.front(); const auto carrier = fleet.members.front().id;
        movement.contents = {{carrier, {{30000, 1001}}}};
        config.census->objects = {{carrier, 1002, 1, fleet.orbit, fleet.id, false, false, 1000},
            {30000, 1001, 1, fleet.orbit, carrier, false, false, 1001}};
        config.census->objects.front().paid_cost = f(151); // Discount/markup differs from authored 101.
        if (landed) {
            config.fleets.clear(); config.census->objects.front().container = fleet.orbit;
            config.control.emplace(); auto& control = *config.control;
            control.change_seconds = f(7); control.max_ground_forces = 10;
            control.players = {{1, true, false, config.players.front().faction_id}};
            control.relations = {{1, 1, true}}; control.types = {{1000, 0, false, true}};
            for (const auto& planet : config.planets) {
                gc::ControlPlanet row; row.planet = planet.id; row.type = planet.id; row.surface_accessible = true;
                if (planet.id == fleet.orbit) row.landed.push_back({carrier, 1000, 1});
                control.planets.push_back(row);
            }
            for (const auto& route : config.routes) control.routes.push_back({route.id, f(1)});
        }
        return config;
    };
    const auto query = [&](const auto& config) {
        auto game = take(gc::GalacticSession::create(config)); const auto snapshot = game.snapshot();
        const auto& actor = config.census->objects.front();
        return gc::object_sale(*snapshot->production(), snapshot->sale_types(), snapshot->actor_records(), snapshot->fleets(),
            snapshot->control(), snapshot->movement(), 1, actor.planet, actor.id);
    };
    {
        auto built = config_for(true);
        built.census->objects.clear(); built.movement->contents.clear();
        for (auto& planet : built.control->planets) { planet.landed.clear(); planet.ground_base = {1, 1}; }
        built.production->nested_creation.emplace(); auto& nested = *built.production->nested_creation;
        nested.fleet_type = 1000;
        nested.types = {{1000, 0, gc::NestedCreationRole::transport, {}, false, false, gc::ActorCombatRole::unavailable, {}},
            {1001, 0, gc::NestedCreationRole::other, {}, false, false, gc::ActorCombatRole::unavailable, {}},
            {1002, 1000, gc::NestedCreationRole::company, {1001}, false, false, gc::ActorCombatRole::unavailable, {}}};
        nested.relations = {{1, 1, false}};
        const auto planet_id = built.planets.front().id;
        auto& producer = built.production->planets.front(); auto product = producer.offers.front();
        product.type = 1002; product.population = f(3); product.price = f(151); product.authored_price = f(101);
        producer.offers.push_back(product);
        eawr::sim::InlineExecutor executor;
        auto game = take(campaign::GameSession::create(built));
        (void)take(game.submit(buy(0, 1, planet_id, 1002)));
        (void)take(game.step(executor));
        const auto started = game.galaxy().snapshot();
        for (const auto& event : started->events()) if (event.kind == gc::GalacticEventKind::command_rejected)
            std::cerr << "nested fixture command rejection: " << static_cast<unsigned>(event.reason) << "/" << event.value << '\n';
        expect(started->production()->planets.front().queues[0].size() == 1, "nested sale fixture admits its company purchase");
        expect(started->player_economy()->players.front().credits == f(1849), "nested company purchase debits its actual price");
        for (int i = 0; i < 3; ++i) (void)take(game.step(executor));
        const auto snapshot = game.galaxy().snapshot();
        expect(snapshot->production()->planets.front().queues[0].empty(), "nested sale fixture finishes production before querying its identity");
        const auto actor = std::find_if(snapshot->actor_records().begin(), snapshot->actor_records().end(),
            [](const auto& row) { return row.original_type == 1002; });
        expect(actor != snapshot->actor_records().end() && actor->paid_cost == f(151), "nested completion retains the actual paid cost on the company identity");
        if (actor != snapshot->actor_records().end()) {
            const auto sale = gc::object_sale(*snapshot->production(), snapshot->sale_types(), snapshot->actor_records(), snapshot->fleets(),
                snapshot->control(), snapshot->movement(), 1, planet_id, actor->id);
            expect(sale.admission == gc::ProductionAdmission::allowed && sale.refund == f(76), "newly built company sale uses actual cost, not authored price");
            const auto identity = cp::game_identities(built);
            const auto saved = take(cp::save_game_checkpoint(game, identity));
            auto restored = take(cp::load_game_checkpoint(saved, identity, {}, executor));
            expect(take(cp::save_game_checkpoint(restored.session, identity)) == saved, "nested-produced paid cost checkpoint round trip is canonical");
            const gc::GameCommand sell{{game.completed_tick(), {1}, 2}, gc::SellObject{planet_id, actor->id}};
            (void)take(game.submit(sell)); (void)take(restored.session.submit(sell));
            (void)take(game.step(executor)); (void)take(restored.session.step(executor));
            expect(game.galaxy().snapshot()->player_economy()->players.front().credits == f(1925), "nested company sale credits retained paid cost after production");
            expect(game.galaxy().snapshot()->sha256() == restored.session.galaxy().snapshot()->sha256(), "nested cost refund after restore equals uninterrupted sale");
        }
    }
    auto config = config_for(false);
    expect(query(config).refund == f(76), "unit refund uses retained actual cost, half rounded up");
    auto hero = config; hero.census->types[1].named_hero = true;
    expect(query(hero).admission == gc::ProductionAdmission::unavailable, "default hero cargo prevents sale");
    hero.census->types[1].sale_policy = 1;
    expect(query(hero).admission == gc::ProductionAdmission::allowed, "authored Always permits hero cargo");
    hero.census->types[1].sale_policy = 2;
    expect(query(hero).admission == gc::ProductionAdmission::unavailable, "authored Never cargo refuses sale");
    auto never = config; never.census->types.back().sale_policy = 2;
    expect(query(never).admission == gc::ProductionAdmission::unavailable, "true company Never overrides sellable transport");
    auto free = config; free.census->objects.front().paid_cost = {};
    expect(query(free).refund == f(50), "starting company refund uses original authored integer price");
    auto expensive = free; expensive.census->types.back().authored_price = f(100000000);
    expect(query(expensive).refund == f(50000000), "a starting type absent from production offers keeps its represented authored price without the paid-cost limit");
    auto foreign = config; foreign.census->objects.front().owner = 0;
    expect(query(foreign).admission != gc::ProductionAdmission::allowed, "unowned identity refuses sale");
    std::vector<std::string> baseline;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        std::vector<std::string> hashes;
        for (bool landed : {false, true}) {
            auto source = config_for(landed); const auto actor = source.census->objects.front();
            gc::GalacticReplay replay{source, 3, {{{1, {1}, 1}, gc::SellObject{actor.planet, actor.id}},
                {{1, {1}, 2}, gc::SellObject{actor.planet, actor.id}}}};
            expect(take(gc::parse_replay(take(gc::write_replay(replay)))) == replay, "unit-sale replay retains costs and command");
            auto game = take(campaign::GameSession::from_replay(replay)); (void)take(game.step(pool));
            const auto identity = cp::game_identities(source); const auto bytes = take(cp::save_game_checkpoint(game, identity));
            auto restored = take(cp::load_game_checkpoint(bytes, identity, {}, pool));
            expect(take(cp::save_game_checkpoint(restored.session, identity)) == bytes, "pre-sale checkpoint retains actual cost canonically");
            const auto old = game.galaxy().snapshot();
            (void)take(game.step(pool)); (void)take(restored.session.step(pool));
            const auto snapshot = game.galaxy().snapshot();
            expect(snapshot->player_economy()->players.front().credits == f(2076), "unit and all cargo refund only the selected identity once");
            expect(snapshot->population().front().used == f(0), "sale releases company population");
            expect(snapshot->fleets().empty() && snapshot->movement()->contents.empty(), "sale releases payload and empty fleet shell");
            expect(std::none_of(snapshot->actor_records().begin(), snapshot->actor_records().end(),
                [&](const auto& row) { return row.id == actor.id || row.id == 30000; }), "sale removes cargo actors");
            expect(old->movement()->contents.size() == 1, "sale preserves prior snapshot payload");
            if (landed) expect(std::all_of(snapshot->control()->planets.begin(), snapshot->control()->planets.end(),
                [](const auto& row) { return row.landed.empty(); }), "landed sale clears surface presence");
            expect(snapshot->events().back().kind == gc::GalacticEventKind::command_rejected, "second sale rejects stale identity");
            expect(snapshot->sha256() == restored.session.galaxy().snapshot()->sha256(), "sale after restore matches uninterrupted run");
            hashes.push_back(snapshot->sha256());
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "ground and orbital sale deterministic on 1/2/4/8");
    }
}

void census_admission() {
    eawr::sim::InlineExecutor serial;
    auto config = census_setup(); auto game = take(galactic::GalacticSession::create(config));
    expect(game.snapshot()->population()[0] == galactic::PopulationTotal{1, f(2), f(1), f(0), true}, "initial live fleet counts before first purchase");
    submit(game, buy(0, 1)); submit(game, buy(0, 2, 2));
    const auto tick = take(game.step(serial));
    expect(tick.snapshot->events().back().value == static_cast<std::uint64_t>(galactic::ProductionAdmission::population), "second planet cannot over-reserve shared cap");
    expect(game.snapshot()->population()[0].reserved == f(1), "queued unit reserves population once");
    submit(game, cancel(1, 3, 0)); submit(game, buy(1, 4, 2)); (void)take(game.step(serial));
    expect(game.snapshot()->production()->planets[1].queues[0].size() == 1, "cancel releases room for later command in same frame");
    expect(game.snapshot()->population()[0].reserved == f(1), "replacement reservation remains once");
    for (int i = 0; i < 3; ++i) (void)take(game.step(serial));
    expect(game.snapshot()->population()[0].used == f(2) && game.snapshot()->population()[0].reserved == f(0), "completion converts reservation into live usage");
    submit(game, buy(5, 5)); const auto full = take(game.step(serial));
    expect(full.snapshot->events().back().value == static_cast<std::uint64_t>(galactic::ProductionAdmission::population), "at-cap admission refused after completion");
    const auto idle = take(game.step(serial));
    expect(idle.work.population_objects == 0 && idle.work.population_producers == 0, "unchanged idle frame does no census scan");
    config = census_setup(); config.census->players[0].base_capacity = 0;
    auto over = take(galactic::GalacticSession::create(config)); submit(over, buy(0, 1, 1, 1001));
    (void)take(over.step(serial)); expect(over.snapshot()->production()->planets[0].queues[0].size() == 1, "zero-pop structure admitted while over cap");
    config = census_setup(); config.rules.fiscal_cycle_frames = 1000;
    config.census->types[0].additional_capacity = -5;
    config.production->players[0].population_capacity = f(-3);
    auto negative = take(galactic::GalacticSession::create(config));
    expect(negative.snapshot()->population()[0].capacity == f(-3), "RGC-142 capacity remains negative after the player-wide sum");
    submit(negative, buy(0, 1));
    const auto negative_refused = take(negative.step(serial));
    expect(negative_refused.snapshot->events().back().value == static_cast<std::uint64_t>(galactic::ProductionAdmission::population)
        && negative.snapshot()->production()->planets[0].queues[0].empty()
        && negative.snapshot()->player_economy()->players[0].credits == f(2000),
        "RGC-143/144 negative capacity rejects positive population before queue insertion and payment");
    submit(negative, buy(1, 2, 1, 1001)); (void)take(negative.step(serial));
    expect(negative.snapshot()->production()->planets[0].queues[0].size() == 1,
        "RGC-143 negative capacity still admits zero population");
    config = census_setup(); config.production->planets[0].offers[0].current_limit = 1;
    auto limited = take(galactic::GalacticSession::create(config)); submit(limited, buy(0, 1));
    const auto refused = take(limited.step(serial));
    expect(refused.snapshot->events().back().value == static_cast<std::uint64_t>(galactic::ProductionAdmission::current_limit), "current limit sees initial live fleet without supplied counts");
    config = census_setup(); config.production->planets[0].offers[0].domain = galactic::ProductionDomain::space;
    auto orbital = take(galactic::GalacticSession::create(config)); submit(orbital, buy(0, 1, 1, 1000, galactic::ProductionDomain::space));
    for (int i = 0; i < 4; ++i) (void)take(orbital.step(serial));
    expect(orbital.snapshot()->population()[0].used == f(2), "orbital paid-result mirror and fleet member share one census identity");
}

void census_campaign_retained_counts() {
    namespace tactical = eawr::sim::tactical;
    auto config = census_setup();
    config.players.push_back({2, 2, 0});
    config.seats.push_back({{2}, 2, galactic::SeatKind::ai, 0, true, 0});
    config.player_economy->players.push_back({2, f(2000), f(10000), 2, 5});
    config.fiscal->players.push_back({2, false, f(1)});
    config.production->players.push_back({2, false, true, true, f(50), {}, {}, {}, {}});
    config.census->players[0].base_capacity = 100;
    config.census->players.push_back({2, 100, false, {}});
    config.fleets[0].members.push_back({30000, 1000});
    galactic::Fleet defender_fleet;
    defender_fleet.id = 40000; defender_fleet.owner = 2; defender_fleet.orbit = config.fleets[0].orbit;
    defender_fleet.members = {{40001, 1000}};
    config.fleets.push_back(std::move(defender_fleet));
    for (auto& planet : config.production->planets) planet.offers[0].current_limit = 2;

    // WGEP-78: campaign retained and reserve records refer to these same owned
    // IDs, not extra entries in a type-only purchase reinforcement pool.
    campaign::ConflictRecord conflict;
    auto frontier = take(galactic::GalacticSession::create(config));
    conflict.id = frontier.next_entity_id(); conflict.planet = config.fleets[0].orbit;
    conflict.invader = 1; conflict.defender = 2;
    conflict.forces = {{config.fleets[0].members[0].id, 1000, 1, config.fleets[0].id, campaign::ForceRole::retained},
        {30000, 1000, 1, config.fleets[0].id, campaign::ForceRole::reinforcement},
        {40001, 1000, 2, 40000, campaign::ForceRole::deployed}};
    conflict.seats = {{{1}, 1}, {{2}, 2}};
    conflict.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    conflict.tactical.first_free_entity_id = conflict.id + 1;
    tactical::UnitState defender; defender.entity_id = 40001; defender.type_id = 1000; defender.owner = 2;
    conflict.tactical.units.push_back(defender);
    tactical::TacticalContent content;
    tactical::DurabilityProfile profile; profile.type_id = 1000; profile.max_hull = f(100);
    content.durability.profiles.push_back(profile);
    content.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    content.victory.contenders = {1, 2}; content.victory.controlled_players = {1, 2};
    content.victory.humans = {1}; content.victory.relevant_types = {1000}; content.victory.countdown_frames = 1000;
    cp::GameLoadOptions options; options.script_requirement = eawr::campaign::ScriptRequirement::none;
    options.content = [content](const auto&) { return eawr::core::Result<tactical::TacticalContent>::success(content); };
    const auto identity = cp::game_identities(config);
    const auto owned = [](const campaign::GameSession& game) {
        const auto& counts = game.galaxy().snapshot()->production()->players[0].counts;
        const auto at = std::find_if(counts.begin(), counts.end(), [](const auto& row) { return row.type == 1000; });
        return at == counts.end() ? 0 : at->owned;
    };
    const auto blocked = [](const campaign::GameTick& tick) {
        return std::any_of(tick.galaxy->events().begin(), tick.galaxy->events().end(), [](const auto& event) {
            return event.kind == galactic::GalacticEventKind::command_rejected
                && event.value == static_cast<std::uint64_t>(galactic::ProductionAdmission::current_limit);
        });
    };
    std::vector<std::string> reference;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(campaign::GameSession::create(config));
        expect(owned(game) == 2, "campaign retained and reserve fleet identities count once, owner only");
        if (auto result = game.submit(buy(0, 1)); !result) throw std::runtime_error(result.error().message);
        auto tick = take(game.step(pool));
        expect(blocked(tick) && game.galaxy().snapshot()->production()->planets[0].queues[0].empty(),
            "retained campaign force reaches current limit before tactical entry");
        std::vector<std::string> hashes{tick.state_hash.get()};
        auto saved = take(cp::save_game_checkpoint(game, identity));
        auto restored = take(cp::load_game_checkpoint(saved, identity, {}, pool));
        expect(take(cp::save_game_checkpoint(restored.session, identity)) == saved, "retained owned census checkpoint is canonical");
        for (auto* session : {&game, &restored.session})
            if (auto result = session->submit(buy(1, 2)); !result) throw std::runtime_error(result.error().message);
        tick = take(game.step(pool)); const auto loaded_tick = take(restored.session.step(pool));
        expect(blocked(tick) && blocked(loaded_tick) && loaded_tick.state_hash.get() == tick.state_hash.get(),
            "retained campaign identity still blocks a purchase after load");
        hashes.push_back(tick.state_hash.get());
        if (auto staged = game.stage_battle(conflict, content, eawr::campaign::ScriptRequirement::none); !staged) throw std::runtime_error(staged.error().message);
        hashes.push_back(take(game.step(pool)).state_hash.get());
        expect(game.mode() == campaign::SessionMode::battle && owned(game) == 2,
            "active retained and reserve conflict records do not double-count galactic identities");
        saved = take(cp::save_game_checkpoint(game, identity));
        auto battle_loaded = take(cp::load_game_checkpoint(saved, identity, options, pool));
        expect(battle_loaded.session.mode() == campaign::SessionMode::battle && owned(battle_loaded.session) == 2,
            "active conflict load retains both roles and their exact galactic owned count");
        expect(take(cp::save_game_checkpoint(battle_loaded.session, identity)) == saved,
            "active retained-force checkpoint is canonical");
        for (int i = 0; i < 3; ++i) {
            const auto next = take(game.step(pool)); hashes.push_back(next.state_hash.get());
            expect(take(battle_loaded.session.step(pool)).state_hash.get() == hashes.back(), "active retained-force load continuation equality");
        }
        if (workers == 1) reference = hashes; else expect(reference == hashes, "campaign retained census 1/2/4/8 equality");
    }
    config.production->planets[0].offers[0].current_limit = 3;
    eawr::sim::InlineExecutor serial;
    auto room = take(galactic::GalacticSession::create(config)); submit(room, buy(0, 1)); (void)take(room.step(serial));
    expect(room.snapshot()->production()->planets[0].queues[0].size() == 1,
        "two campaign-backed identities leave one slot at limit three");
}

void company_original_binding() {
    auto config = census_setup();
    config.fleet_control->types[0].transport = true;
    config.census->types.push_back({1002, f(3), 0, {1000, 1001}, true, 1000});
    config.movement.emplace(); auto& movement = *config.movement;
    movement.mode_modifier = f(1);
    movement.types = {{1000, f(3), {}, 0, true, false, false, false, true},
        {1001, {}, {}, 0, false, false, false, false}};
    movement.players = {{1, f(1), {}, false}};
    for (const auto& planet : config.planets) {
        movement.planets.push_back({planet.id, {}, false});
        movement.access.push_back({1, planet.id, true, false});
    }
    const auto& fleet = config.fleets[0]; const auto carrier = fleet.members[0].id;
    movement.contents = {{carrier, {{30000, 1001}}}};
    config.census->objects = {{carrier, 1002, 1, fleet.orbit, fleet.id, false, false},
        {30000, 1001, 1, fleet.orbit, carrier, false, false}};
    expect(bool(galactic::validate_setup(config)), "exact authored company/transport binding admits retained original company identity");
    auto invalid = config; invalid.census->types.back().company = false; invalid.census->types.back().transport = 0;
    expect(!galactic::validate_setup(invalid), "ordinary census identity overrides still refuse");
    invalid = config; invalid.census->types.back().transport = 1001;
    expect(!galactic::validate_setup(invalid), "another authored member is not the exact runtime transport");
    invalid = config; invalid.census->types.back().transport = 9999;
    expect(!galactic::validate_setup(invalid), "unbound company transport refuses");
    invalid = config; invalid.movement->types[0].transport = false; invalid.movement->contents.clear();
    invalid.census->objects[0].original_type = 1000; invalid.census->objects.pop_back();
    expect(!galactic::validate_setup(invalid), "dormant company binding cannot name an ordinary movement type");
    invalid = config; invalid.movement->contents[0].children.clear(); invalid.census->objects.pop_back();
    expect(!galactic::validate_setup(invalid), "company original identity requires nonempty explicit transport contents");
    invalid = config; invalid.census->types.push_back({1003, {}, 0});
    invalid.movement->types.push_back({1003, {}, {}, 0, false, false, false, false});
    invalid.movement->contents[0].children[0].type = 1003; invalid.census->objects[1].original_type = 1003;
    expect(!galactic::validate_setup(invalid), "unrelated carried type cannot acquire company original identity");
    galactic::GalacticReplay replay{config, 4, {}};
    const auto bytes = take(galactic::write_replay(replay));
    expect(take(galactic::parse_replay(bytes)) == replay, "company original/runtime binding tail round trips");
    const auto integer = [&](std::size_t offset, std::size_t width) {
        std::uint32_t value{};
        for (std::size_t i = 0; i < width; ++i) value |= std::uint32_t{bytes.at(offset + i)} << (8 * i);
        return value;
    };
    std::size_t offset = 100;
    for (std::uint32_t i = 0; i < integer(96, 4); ++i) {
        const auto tag = integer(offset, 2), size = integer(offset + 2, 4);
        if (tag == 10) {
            const auto tail = offset + 6 + size - 21;
            auto corrupt = bytes; corrupt[tail] = 2;
            expect(!galactic::parse_replay(corrupt), "unknown census company-tail version refuses");
            corrupt = bytes; corrupt[tail + 1] = 0;
            expect(!galactic::parse_replay(corrupt), "empty census company-tail count refuses");
            corrupt = bytes; corrupt[tail + 5] = 99;
            expect(!galactic::parse_replay(corrupt), "unbound census original company type refuses");
        }
        offset += 6 + size;
    }
    std::vector<std::string> reference;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(campaign::GameSession::from_replay(replay));
        expect(game.galaxy().snapshot()->population()[0].used == f(3), "population reads original company, not its runtime transport type");
        const auto identity = cp::game_identities(config);
        const auto saved = take(cp::save_game_checkpoint(game, identity));
        auto loaded = take(cp::load_game_checkpoint(saved, identity, {}, pool));
        expect(take(cp::save_game_checkpoint(loaded.session, identity)) == saved, "tick-zero company checkpoint is canonical");
        std::vector<std::string> hashes;
        for (unsigned i = 0; i < 4; ++i) {
            hashes.push_back(take(game.step(pool)).state_hash.get());
            expect(take(loaded.session.step(pool)).state_hash.get() == hashes.back(), "company original metadata survives restored continuation");
        }
        if (workers == 1) reference = hashes; else expect(reference == hashes, "company original/runtime census equal on 1/2/4/8 workers");
    }
    eawr::units::GalacticType type; type.name = "Company"; type.behavior = {"DUMMY_GROUND_COMPANY"};
    type.company_transport = "Carrier"; type.company_units = {"Troop"};
    const std::vector<campaign::FleetTypeBinding> bindings{{1000, "Carrier"}, {1001, "Troop"}};
    const auto prepared = take(campaign::prepare_census_type(type, 1002, bindings));
    expect(prepared.company && prepared.transport == 1000, "company binding is sourced from original behaviour and authored transport link");
    type.company_transport.clear();
    expect(!campaign::prepare_census_type(type, 1002, bindings), "missing authored company transport cannot become a guessed binding");
}

void census_container_and_capacity() {
    eawr::sim::InlineExecutor serial; auto config = census_setup();
    config.census->types.push_back({2000, f(3), 5});
    config.census->types.push_back({2001, f(0), 2});
    config.census->types.push_back({2002, f(0), 7});
    config.census->objects = {
        {1, 2001, 1, 1, 0, false, false},
        {30000, 2000, 1, 1, 0, false, false},
        {30001, 1000, 1, 1, 30000, false, false},
        {30002, 2001, 1, 1, 30000, false, false},
        {30003, 1000, 1, 1, 30002, false, false},
        {30004, 2000, 1, 1, 0, true, false},
        {30005, 2000, 1, 1, 0, false, true},
        {30006, 1000, 1, 1, 30004, false, false}};
    config.production->planets[0].starbase_type = 2002;
    auto game = take(galactic::GalacticSession::create(config));
    const auto total = game.snapshot()->population()[0];
    expect(total.used == f(11), "dead/delete-pending wrappers still count; child suppression reads immediate original only");
    expect(total.capacity == f(18), "capacity skips dead/deleting sources; counts owned planet, live containers and station");
    const auto& counts = game.snapshot()->production()->players[0].counts;
    const auto wrapper_count = std::find_if(counts.begin(), counts.end(), [](const auto& row) { return row.type == 2000; });
    expect(wrapper_count != counts.end() && wrapper_count->owned == 3, "WGEP-78 current counts retain live, dead and delete-pending owned identities");
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto parallel = take(galactic::GalacticSession::create(config));
        expect(take(parallel.step(pool)).state_hash.get() == take(game.step(serial)).state_hash.get(), "container census deterministic across partition executors");
        game = take(galactic::GalacticSession::create(config));
    }
    config.seats[0].kind = galactic::SeatKind::ai;
    config.seats[0].human_side_id = 0;
    config.production->players[0].human = false; config.production->players[0].pay_as_you_go = true;
    config.census->players[0].unusable_planets = {1};
    auto ai = take(galactic::GalacticSession::create(config));
    expect(ai.snapshot()->population()[0].capacity == f(0) && ai.snapshot()->population()[0].used == f(0)
        && ai.snapshot()->population()[0].reserved == f(0) && !ai.snapshot()->population()[0].uses_cap
        && galactic::population_admitted(ai.snapshot()->population()[0], f(100)), "omitted AI cap false reports zero capacity and usage while bypassing admission");
    config.census->players[0].ai_uses_cap = true;
    auto capped = take(galactic::GalacticSession::create(config));
    expect(capped.snapshot()->population()[0].used == f(0) && capped.snapshot()->population()[0].capacity == f(2), "capped AI skips objects and station under unusable planet");
    expect(!galactic::population_admitted(capped.snapshot()->population()[0], f(3)), "enabled AI cap applies room check");
    config.census->objects[4].planet = 2; // Grandchild must follow its two containers to planet 1.
    config.census->objects[4].original_type = 2000;
    auto enclosed = take(galactic::GalacticSession::create(config));
    expect(enclosed.snapshot()->population()[0].used == f(0)
        && enclosed.snapshot()->population()[0].capacity == f(2), "AI enclosing-planet traversal is independent of immediate population suppression");
    config.census->objects[1].container = 30003;
    expect(!galactic::GalacticSession::create(config), "cyclic census containment rejected without an unbounded traversal");
    config = census_setup();
    config.census->types.push_back({2000, f(1), 5});
    config.census->objects = {{1, 1001, 1, 1, 30000, false, false},
        {30000, 2000, 1, 1, 0, false, false}};
    auto contained_producer = take(galactic::GalacticSession::create(config));
    submit(contained_producer, buy(0, 1)); (void)take(contained_producer.step(serial));
    expect(contained_producer.snapshot()->population()[0].reserved == f(0)
        && contained_producer.snapshot()->population()[0].capacity == f(7), "positive immediate parent suppresses producer queues while retaining live capacity");
    config = census_setup(); config.fleet_control->types[0].transport = true;
    config.census->types[1].population = f(3);
    for (auto& planet : config.production->planets) planet.offers[1].population = f(3);
    const auto transport_id = config.fleets[0].members[0].id;
    config.census->objects = {{transport_id, 1000, 1, 1, 0, false, false},
        {30000, 1001, 1, 1, transport_id, false, false}};
    auto transport = take(galactic::GalacticSession::create(config));
    expect(transport.snapshot()->population()[0].used == f(1), "positive original transport population suppresses its cargo");
    submit(transport, {{0, {1}, 1}, galactic::RemoveFleetMembers{config.fleets[0].id, {transport_id}}});
    (void)take(transport.step(serial));
    expect(transport.snapshot()->population()[0].used == f(1), "detaching transport keeps one identity and valid immediate cargo container");
    config.census->types[0].population = f(0);
    for (auto& planet : config.production->planets) planet.offers[0].population = f(0);
    auto empty_wrapper = take(galactic::GalacticSession::create(config));
    expect(empty_wrapper.snapshot()->population()[0].used == f(3), "zero original transport population leaves cargo counted");
}

void census_rounding_and_data() {
    eawr::sim::InlineExecutor serial; auto config = census_setup();
    const auto fraction = take(Fixed::from_decimal("0.4"));
    config.census->types[0].population = fraction;
    for (auto& planet : config.production->planets) planet.offers[0].population = fraction;
    auto same = take(galactic::GalacticSession::create(config)); submit(same, buy(0, 1)); submit(same, buy(0, 2));
    (void)take(same.step(serial)); expect(same.snapshot()->population()[0].reserved == f(1), "producer sum rounds after all queue entries");
    for (auto& planet : config.production->planets) planet.offers[1].population = fraction;
    config.census->types[1].population = fraction;
    config.production->planets[0].offers[1].domain = galactic::ProductionDomain::space;
    auto both = take(galactic::GalacticSession::create(config)); submit(both, buy(0, 1));
    submit(both, buy(0, 2, 1, 1001, galactic::ProductionDomain::space)); (void)take(both.step(serial));
    expect(both.snapshot()->population()[0].reserved == f(1), "ground and space queue sums round together");
    auto separate = take(galactic::GalacticSession::create(config)); submit(separate, buy(0, 1)); submit(separate, buy(0, 2, 2));
    (void)take(separate.step(serial)); expect(separate.snapshot()->population()[0].reserved == f(0), "different producer fractional sums round separately");
    config.census->objects.push_back({30000, 1000, 1, 1, 0, false, false});
    auto fractional_live = take(galactic::GalacticSession::create(config));
    expect(fractional_live.snapshot()->population()[0].used == f(1), "global live usage rounds once after summation");
    eawr::units::GalacticType type; type.population_value = f(3); type.additional_population_capacity = 7;
    galactic::CensusType expected_type{10, f(3), 7}; expected_type.history = galactic::HistoryType{};
    expect(take(campaign::prepare_census_type(type, 10)) == expected_type, "original type adapter consumes loaded values");
    for (const auto capacity : {std::numeric_limits<std::int32_t>::min(), -5, std::numeric_limits<std::int32_t>::max()}) {
        type.additional_population_capacity = capacity;
        expect(take(campaign::prepare_census_type(type, 10)).additional_capacity == capacity,
            "RGC-140 adapter preserves the full signed 32-bit capacity range");
        auto signed_setup = census_setup(); signed_setup.census->types[0].additional_capacity = capacity;
        expect(bool(galactic::validate_setup(signed_setup)), "RGC-140 census admits signed 32-bit capacity");
    }
    type.additional_population_capacity = std::int64_t{1} << 31;
    expect(!campaign::prepare_census_type(type, 10), "capacity outside signed source integer range is refused");
    type.additional_population_capacity = 7;
    type.population_value = take(Fixed::from_decimal("3.9"));
    expect(take(campaign::prepare_census_type(type, 10)).population == f(3), "authored population uses the source integer overlay");
    type.population_value.reset(); type.additional_population_capacity.reset();
    expected_type.population = f(0); expected_type.additional_capacity = 0;
    expect(take(campaign::prepare_census_type(type, 10)) == expected_type, "WGEP-69 omitted type values default zero after inheritance");
    eawr::data::galactic::Planet planet; planet.population_value = 2; planet.additional_population_capacity = 8;
    expect(take(campaign::prepare_census_planet(planet, 11)) == galactic::CensusType{11, f(2), 8}, "planet original population and capacity use their own loaded table");
    eawr::data::galactic::Faction faction;
    expect(campaign::prepare_census_player(faction, 1, {}).base_capacity == 0
        && !campaign::prepare_census_player(faction, 1, {}).ai_uses_cap, "verified omitted faction base/AI defaults");
    faction.base_population_capacity = 12; faction.ai_uses_galactic_pop_cap = true;
    expect(campaign::prepare_census_player(faction, 1, {}).base_capacity == 12
        && campaign::prepare_census_player(faction, 1, {}).ai_uses_cap, "explicit faction policy wins");
    config.census->objects[0].container = 999999;
    expect(!galactic::validate_setup(config), "missing immediate container is rejected");
}

void census_stock_composite_boundary() {
    std::string root = eawr::testing::game_root().value_or("");
    if (root.empty()) return;
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(root) / folder / "Data")).mount);
    auto filesystem = take(eawr::vfs::Vfs::mount(mounts));
    const auto catalog = take(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    const auto* squadron = types.find("Rebel_X-Wing_Squadron");
    expect(squadron && squadron->squadron_units.size() > 1, "stock fighter squadron supplies composite type links");
    if (!squadron) return;
    const std::vector<campaign::FleetTypeBinding> binding{{1000, squadron->name}};
    auto traits = take(campaign::build_fleet_type_traits(types, binding));
    expect(traits[0].requires_live_children, "stock composite fighter needs the unrepresented child service");
    auto config = census_setup(); config.fleet_control->types = std::move(traits);
    config.production->planets[0].offers[0].domain = galactic::ProductionDomain::space;
    const auto admitted = galactic::validate_production(config);
    expect(!admitted && admitted.error().message.find("orbital result requires supported original fleet traits") != std::string::npos,
        "stock composite production cannot reach current atomic census with an incorrect per-component count");
    auto& denied = config.production->planets[0].offers[0]; denied.direct_build_allowed = false;
    expect(static_cast<bool>(galactic::validate_production(config)), "retained denied composite offer validates without atomic completion traits");
    expect(galactic::production_admission({*config.production, config.production->planets[0],
        config.production->players[0], config.player_economy->players[0], config.fleets,
        config.planets[0].owner, nullptr, 10000, 1}, &denied, galactic::ProductionDomain::space)
        == galactic::ProductionAdmission::unavailable, "shared admission refuses retained composite offer before completion");
    const auto* hero = types.find("Luke_Skywalker_Jedi_Team");
    expect(hero && hero->build_limit_current_per_player && !hero->company_units.empty(), "stock hero team carries a current build limit and children");
    if (!hero) return;
    std::vector<campaign::FleetTypeBinding> hero_bindings;
    for (const auto& row : types.types) hero_bindings.push_back({static_cast<galactic::TypeId>(hero_bindings.size() + 1), row.name});
    const auto hero_id = [&](const std::string& name) {
        const auto* original = types.find(name);
        return static_cast<galactic::TypeId>(original - types.types.data() + 1);
    };
    auto stock = census_setup(); stock.fleets.clear(); stock.census->objects.clear(); stock.census->types.clear();
    std::set<galactic::TypeId> pending{hero_id(hero->name)}, gathered;
    while (!pending.empty()) {
        const auto id = *pending.begin(); pending.erase(pending.begin());
        if (!gathered.insert(id).second) continue;
        auto row = take(campaign::prepare_census_type(types.types[id - 1], id, hero_bindings));
        pending.insert(row.members.begin(), row.members.end()); stock.census->types.push_back(std::move(row));
    }
    std::sort(stock.census->types.begin(), stock.census->types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    // Keep synthetic offer population sourced from its own original fixture types.
    const auto team = hero_id(hero->name);
    stock.production->planets[0].offers = {{}};
    auto& hero_offer = stock.production->planets[0].offers[0];
    hero_offer.type = team; hero_offer.price = f(1); hero_offer.seconds = f(1);
    hero_offer.affiliated = true; hero_offer.direct_build_allowed = true;
    hero_offer.population = take(campaign::prepare_census_type(*hero, team, hero_bindings)).population; hero_offer.current_limit = *hero->build_limit_current_per_player;
    for (std::size_t i = 1; i < stock.production->planets.size(); ++i) stock.production->planets[i].offers.clear();
    stock.census->objects.push_back({30000, hero_id(hero->company_units.front()), 1, 1, 0, false, false});
    eawr::sim::InlineExecutor serial;
    auto hero_game = take(galactic::GalacticSession::create(stock));
    submit(hero_game, buy(0, 1, 1, team));
    expect(take(hero_game.step(serial)).snapshot->events()[0].value == static_cast<std::uint64_t>(galactic::ProductionAdmission::current_limit), "stock hero existing only as children cannot exceed its current limit");
}

void census_composite_counts() {
    auto config = census_setup(); config.census->objects.clear(); config.fleets.clear();
    config.census->types = {{1000, f(1), 0, {1001, 1002}}, {1001, f(0), 0, {1003}}, {1002, f(0), 0}, {1003, f(0), 0}};
    config.census->objects = {{30000, 1000, 1, 1, 0, true, true}, {30001, 1002, 1, 1, 0, false, false},
        {30002, 1003, 1, 1, 30000, true, true}, {30003, 1003, 1, 1, 30000, false, false}};
    config.census->players[0].base_capacity = 100;
    for (auto& planet : config.production->planets) planet.offers[0].current_limit = 3;
    galactic::GalacticReplay replay{config, 4, {buy(0, 1)}};
    const auto bytes = take(galactic::write_replay(replay));
    expect(take(galactic::parse_replay(bytes)) == replay, "composite member tail round trips");
    for (std::size_t n = bytes.size() - 20; n < bytes.size(); ++n) expect(!galactic::parse_replay(std::span(bytes).first(n)), "truncated member tail rejected");
    std::vector<std::string> hashes;
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = take(campaign::GameSession::from_replay(replay));
        const auto& counts = game.galaxy().snapshot()->production()->players[0].counts;
        const auto at = std::find_if(counts.begin(), counts.end(), [](const auto& row) { return row.type == 1000; });
        expect(at != counts.end() && at->owned == 3, "direct original plus recursive MAX counts dead and suppressed children independently of usage");
        const auto tick = take(game.step(pool));
        expect(tick.galaxy->events()[0].kind == galactic::GalacticEventKind::command_rejected
            && tick.galaxy->events()[0].value == static_cast<std::uint64_t>(galactic::ProductionAdmission::current_limit), "composite current limit rejects purchase");
        const auto identity = cp::game_identities(config);
        const auto saved = take(cp::save_game_checkpoint(game, identity));
        auto loaded = take(cp::load_game_checkpoint(saved, identity, {}, pool));
        expect(take(cp::save_game_checkpoint(loaded.session, identity)) == saved, "composite checkpoint canonical round trip");
        std::vector<std::string> actual{tick.state_hash.get()};
        for (int i = 1; i < 4; ++i) {
            const auto next = take(game.step(pool)); actual.push_back(next.state_hash.get());
            expect(take(loaded.session.step(pool)).state_hash.get() == actual.back(), "composite checkpoint continuation equality");
        }
        if (workers == 1) hashes = actual; else expect(hashes == actual, "composite 1/2/4/8 equality");
    }
    auto invalid = config; invalid.census->types[3].members = {1000};
    expect(!galactic::validate_setup(invalid), "cyclic composite graph refused");
    invalid = config; invalid.census->types[3].members = {9999};
    expect(!galactic::validate_setup(invalid), "missing composite member type refused");
    invalid = config; invalid.census->types[0].members = {1002, 1001};
    expect(!galactic::validate_setup(invalid), "unordered composite member graph refused");
    config.production->planets[0].offers[0].current_limit = 4;
    eawr::sim::InlineExecutor serial; auto room = take(galactic::GalacticSession::create(config));
    submit(room, buy(0, 1)); submit(room, buy(0, 2));
    const auto tick = take(room.step(serial));
    expect(room.snapshot()->production()->planets[0].queues[0].size() == 1, "MAX differs from SUM and separately queued parent reserves final limit slot");
    expect(std::any_of(tick.snapshot->events().begin(), tick.snapshot->events().end(), [](const auto& event) {
        return event.kind == galactic::GalacticEventKind::command_rejected && event.value == static_cast<std::uint64_t>(galactic::ProductionAdmission::current_limit);
    }), "second queued composite purchase reaches limit");
}

void census_corpus_checkpoint() {
    eawr::sim::InlineExecutor serial;
    auto config = census_setup();
    galactic::GalacticReplay replay{config, 10, {buy(0, 1), buy(0, 2, 2), cancel(1, 3, 0), buy(1, 4, 2), buy(5, 5)}};
    const auto bytes = take(galactic::write_replay(replay));
    expect(take(galactic::parse_replay(bytes)) == replay, "census replay extension round trip");
    for (std::size_t i = 0; i < bytes.size(); i += 31) expect(!galactic::parse_replay(std::span(bytes).first(i)), "truncated census replay refused");
    auto baseline = take(campaign::GameSession::from_replay(replay)); std::vector<std::string> hashes;
    for (int i = 0; i < 10; ++i) hashes.push_back(take(baseline.step(serial)).state_hash.get());
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        for (std::uint64_t barrier : {1U, 2U, 5U}) {
            auto game = take(campaign::GameSession::from_replay(replay));
            while (game.completed_tick() < barrier) (void)take(game.step(pool));
            const auto identity = cp::game_identities(replay.setup);
            const auto saved = take(cp::save_game_checkpoint(game, identity));
            auto restored = take(cp::load_game_checkpoint(saved, identity, {}, pool));
            expect(take(cp::save_game_checkpoint(restored.session, identity)) == saved, "checkpoint retains live population and queued reservations canonically");
            while (restored.session.completed_tick() < 10) {
                const auto index = restored.session.completed_tick(); restored.session.scramble_storage_for_testing();
                expect(take(restored.session.step(pool)).state_hash.get() == hashes[index], "census replay 1/2/4/8 parity after reservation load");
            }
        }
    }
}

void parity_checkpoint_case(bool seat_policy, bool census) {
    auto replay = corpus();
    if (seat_policy) {
        replay.setup.seat_control.emplace();
        replay.setup.seat_control->teams.push_back({1, {1}, false});
        replay.setup.seat_control->controllers.push_back({1, galactic::FactionControl::human,
            galactic::FactionControl::ai, {}, {}});
        replay.setup.movement.emplace();
        auto& movement = *replay.setup.movement;
        movement.mode_modifier = f(1); // explicit synthetic inputs, not retail defaults
        movement.types = {{1000, f(2), f(100), 0, true, false, false, false}};
        movement.players = {{1, f(1), f(0), false}};
        for (const auto& planet : replay.setup.planets) {
            movement.planets.push_back({planet.id, f(1), false});
            movement.access.push_back({1, planet.id, true, false});
        }
        for (auto& planet : replay.setup.production->planets) planet.offers[0].domain = galactic::ProductionDomain::space;
        for (auto& command : replay.commands) if (auto* order = std::get_if<galactic::ProductionCommand>(&command.payload); order && order->type == 1000)
            order->domain = galactic::ProductionDomain::space;
        auto nested = replay.setup;
        const auto container = nested.fleets[0].members[0].id;
        nested.fleets[0].members[0].type = 9000;
        nested.fleet_control->types.push_back({9000, false, true, true});
        nested.movement->types.push_back({9000, f(0), f(100), 0, false, false, true, false});
        nested.movement->contents.push_back({container, {{10000, 1000}}});
        expect(static_cast<bool>(galactic::validate_setup(nested)), "production and valid movement children coexist");
        nested.census = galactic::PopulationCensus{{{1000, f(1), 0}, {1001, f(0), 0}, {9000, f(0), 0}}, {{1, 2, false, {}}}, {}};
        auto nested_game = take(galactic::GalacticSession::create(nested));
        expect(nested_game.snapshot()->population()[0].used == f(1), "census gathers movement child originals under their zero-population parent");
        nested.census->types[2].population = f(2);
        auto suppressed_game = take(galactic::GalacticSession::create(nested));
        expect(suppressed_game.snapshot()->population()[0].used == f(2), "positive original movement parent suppresses its immediate child population");
        nested.production->planets[0].objects.push_back({10000, 1001, 1, f(100), galactic::ProductionDomain::ground, galactic::ProductionKind::structure});
        expect(!galactic::validate_setup(nested), "production identities cannot reuse a movement child ID");
    }
    if (census) replay.setup.census = galactic::PopulationCensus{
        {{1000, f(1), 0}, {1001, f(0), 0}}, {{1, 50, false, {}}}, {}};
    if (seat_policy && census) {
        // Live control reads every initial member once production publishes actor facts.
        for (const auto& fleet : replay.setup.fleets) for (const auto& member : fleet.members) {
            galactic::CensusObject actor;
            actor.id = member.id; actor.original_type = actor.current_type = member.type;
            actor.owner = fleet.owner; actor.planet = fleet.orbit; actor.container = fleet.id;
            actor.combat_role = galactic::ActorCombatRole::ordinary;
            replay.setup.census->objects.push_back(actor);
        }
        std::sort(replay.setup.census->objects.begin(), replay.setup.census->objects.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
        replay.setup.control.emplace();
        auto& control = *replay.setup.control;
        control.change_seconds = f(7); control.max_ground_forces = 10;
        // Synthetic one-player control provider: its sole player is neutral.
        control.players = {{1, true, false, replay.setup.players[0].faction_id}};
        control.relations = {{1, 1, true}};
        control.types = {{1000, 1, false, false}};
        for (const auto& planet : replay.setup.planets) {
            galactic::ControlPlanet row; row.planet = planet.id; row.type = planet.id;
            row.surface_accessible = true; row.ground_base = {1, 1}; row.starbase = {1, 1};
            control.planets.push_back(row);
        }
        for (const auto& route : replay.setup.routes) control.routes.push_back({route.id, f(1)});
        auto& constructors = replay.setup.production->nested_creation.emplace();
        constructors.fleet_type = 1000;
        constructors.types = {{1000, 0, {}, {}, false, false, galactic::ActorCombatRole::unavailable, {}},
            {1001, 0, {}, {}, false, false, galactic::ActorCombatRole::unavailable, {}}};
        constructors.relations = {{1, 1, false}};
        for (auto& type : constructors.types) type.combat_role = galactic::ActorCombatRole::ordinary;
    }
    // The seat-policy+census case enables every currently supported header tag:
    // graph, fleet, economy, movement, seats, fiscal, production, control and census.
    const auto bytes = take(galactic::write_replay(replay));
    expect(take(galactic::parse_replay(bytes)) == replay, "production setup/action replay round trip");
    for (std::size_t i = 0; i < bytes.size(); i += 29) expect(!galactic::parse_replay(std::span(bytes).first(i)), "truncation is rejected");
    eawr::sim::InlineExecutor serial; auto baseline = take(campaign::GameSession::from_replay(replay));
    std::vector<std::string> hashes;
    for (int i = 0; i < 16; ++i) hashes.push_back(take(baseline.step(serial)).state_hash.get());
    for (auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        for (std::uint64_t barrier : {2U, 4U, 7U}) {
            auto game = take(campaign::GameSession::from_replay(replay));
            while (game.completed_tick() < barrier) (void)take(game.step(pool));
            const auto identity = cp::game_identities(replay.setup); auto bytes_saved = take(cp::save_game_checkpoint(game, identity));
            auto loaded = take(cp::load_game_checkpoint(bytes_saved, identity, {}, pool));
            expect(take(cp::save_game_checkpoint(loaded.session, identity)) == bytes_saved, "half-built queue checkpoint canonical round trip");
            while (loaded.session.completed_tick() < 16) {
                loaded.session.scramble_storage_for_testing(); const auto index = loaded.session.completed_tick();
                const auto tick = take(loaded.session.step(pool));
                expect(tick.state_hash.get() == hashes[index], "1/2/4/8 equality after pause, cancellation and load");
            }
        }
    }
}

void parity_checkpoint() { parity_checkpoint_case(false); parity_checkpoint_case(true); parity_checkpoint_case(true, true); }

void write_corpus(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    for (std::string_view name : {"queues-basic", "queues-orbit", "queues-paygo"}) {
        auto replay = corpus();
        if (name == "queues-orbit") {
            for (auto& planet : replay.setup.production->planets) planet.offers[0].domain = galactic::ProductionDomain::space;
            for (auto& command : replay.commands) if (auto* order = std::get_if<galactic::ProductionCommand>(&command.payload); order && order->type == 1000) order->domain = galactic::ProductionDomain::space;
        }
        if (name == "queues-paygo") replay.setup.production->players[0].pay_as_you_go = true;
        const auto path = directory / (std::string(name) + ".eawr-gcreplay");
        const auto pin_path = directory / (std::string(name) + ".hashes.csv");
        if (std::filesystem::exists(path) || std::filesystem::exists(pin_path)) throw std::runtime_error("refusing to replace existing corpus pins");
        const auto bytes = take(galactic::write_replay(replay));
        std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        auto game = take(campaign::GameSession::from_replay(replay)); eawr::sim::InlineExecutor serial;
        std::ostringstream csv; csv << "tick,sha256\n";
        for (std::uint64_t i = 0; i < replay.final_tick_count; ++i) {
            const auto tick = take(game.step(serial)); csv << tick.completed_tick << ',' << tick.state_hash.get() << '\n';
        }
        std::ofstream pins(pin_path, std::ios::binary); pins << csv.str();
        if (!file || !pins) throw std::runtime_error("could not author new queue corpus");
    }
}
} // namespace galactic_production_test
