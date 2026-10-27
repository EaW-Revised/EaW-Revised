// Qualified synthetic provider proof. Installed content/stock Lua integration
// remains a separate acceptance test; these facts do not stand in for retail.
#include "ai_selector_test_support.hpp"
#include "galactic_ai_world.hpp"
#include "galactic_ai_actions.hpp"
#include "galactic_ai_execution.hpp"
#include "galactic_ai_goal_state.hpp"
#include "galactic_ai_movement_runtime.hpp"
#include "galactic_ai_selection_content.hpp"
#include "galactic_ai_selection_work.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>
#include <set>
#include <stdexcept>

// Test-only allocation probe, armed solely around immutable publication hits.
namespace build_alloc_probe {
std::atomic<bool> armed{false};
std::atomic<std::size_t> calls{0};
}
void* operator new(std::size_t size) {
    if (build_alloc_probe::armed.load(std::memory_order_relaxed))
        build_alloc_probe::calls.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace ai = eawr::script::foc::ai;
namespace foc = eawr::script::foc;
namespace ga = foc::galactic_ai;
namespace gc = eawr::sim::galactic;
namespace auth = eawr::script::authoritative;
using eawr::sim::math::Fixed;
std::string assembly_contract(const eawr::sim::PartitionExecutor&, unsigned, bool sparse = true);
namespace {
void goal_reach_index_contracts() {
    const auto lookup = [](const ai::GalacticGoalReachIndex& index, const std::vector<ai::GalacticGoalReach>& rows, decltype(ai::GalacticGoalReach::player) player,
        gc::PlanetId source, std::string_view policy) -> std::optional<std::vector<gc::PlanetId>> {
        const auto id = ai::goal_reach_policy(ai::upper_case(ai::trimmed(policy)));
        if (!id) return std::nullopt;
        const auto* found = index.find(player, source, *id);
        return found ? std::optional{rows[found->row].production_sites.get()} : std::nullopt;
    };
    // Duplicate keys keep the first published row, row spellings are case-folded, unknown spellings never match.
    const std::vector<ai::GalacticGoalReach> rows = {{2, 7, "Any", {5}}, {1, 7, "Friendly_Only", {9, 3}}, {1, 7, "FRIENDLY_ONLY", {4}},
        {1, 7, "Mystery", {1}}, {1, 6, "Single_Hop_Disconnected", {}}, {1, 7, "ANY", {2}}};
    const auto index = ai::GalacticGoalReachIndex::build(rows);
    expect(index.entries.size() == 5, "unrecognised policy spellings stay out of the goal-reach index");
    expect(lookup(index, rows, 1, 7, " friendly_only ") == std::vector<gc::PlanetId>({9, 3}), "first row of a duplicated key wins, in published order");
    expect(lookup(index, rows, 1, 7, "any") == std::vector<gc::PlanetId>({2}) && lookup(index, rows, 2, 7, "ANY") == std::vector<gc::PlanetId>({5}),
        "goal-reach lookup separates players");
    expect(lookup(index, rows, 1, 6, "Single_Hop_Disconnected") == std::vector<gc::PlanetId>{}, "an empty site list is a known row");
    expect(!lookup(index, rows, 1, 6, "Any") && !lookup(index, rows, 1, 8, "Any") && !lookup(index, rows, 3, 7, "Any"), "missing rows report no relation");
    expect(!lookup(index, rows, 1, 7, "Mystery"), "unaccepted policy has no lookup id");
    expect(ai::GalacticGoalReachIndex{}.find(1, 7, ai::GoalReachPolicy::any) == nullptr, "empty index finds nothing");
}
void shared_planet_list_contracts() {
    const ai::SharedPlanetList none, listed{4, 7, 9};
    const auto copy = listed;
    expect(none.empty() && none.size() == 0 && none.begin() == none.end() && ai::SharedPlanetList(std::vector<gc::PlanetId>{}).empty(),
        "an empty list holds no storage");
    expect(copy.begin() == listed.begin() && copy == listed && copy.size() == 3 && copy.front() == 4,
        "a copy shares the ids of the list it was made from");
    expect(listed == ai::SharedPlanetList(std::vector<gc::PlanetId>{4, 7, 9}) && listed.begin() != ai::SharedPlanetList{4, 7, 9}.begin()
        && !(listed == ai::SharedPlanetList{4, 7}) && !(listed == none), "equality compares the ids, not the storage");
    ai::SharedPlanetList cleared = listed; cleared.clear();
    expect(cleared.empty() && listed.size() == 3, "clearing a row's list leaves the sharers untouched");
    const std::vector<gc::PlanetId>& ids = listed;
    expect(ids == std::vector<gc::PlanetId>({4, 7, 9}) && std::find(listed.begin(), listed.end(), 7) != listed.end(),
        "a shared list reads as the ordered vector it replaces");
}
void starbase_proposal_contracts() {
    ai::GalacticSelectionContent content;
    for (const auto& [id, name, level, next] : {
        std::tuple{100ULL, "MARKER", 1, ""}, std::tuple{200ULL, "MOD_L1", 1, "mod_l2"},
        std::tuple{300ULL, "MOD_L2", 2, ""}, std::tuple{400ULL, "OTHER_FACTION_L1", 1, ""}}) {
        ai::GalacticSelectionContentType type; type.name = name; type.starbase = true;
        auto original = std::make_shared<eawr::units::GalacticType>();
        original->name = name; original->base_level = level; original->next_level_base = next;
        type.original = original; content.types.emplace(id, std::move(type));
    }
    content.types.at(200).next_base = 300;
    content.initial_starbase_types = {{1, 200}, {2, 400}};
    gc::PlanetProduction target; target.target_owner = target.starbase_owner = 1;
    target.starbase_type = 200; target.space_level = 1; target.maximum_space_level = 5;
    expect(must(ai::galactic_starbase_proposal(content, &target, 100)) == 300,
        "WGA-481 station marker follows the current station's mod-authored next type");
    content.types.at(200).next_base = 0;
    expect(must(ai::galactic_starbase_proposal(content, &target, 100)) == 100,
        "WGA-483 unresolved nonempty next-level name keeps the authored marker");
    content.types.at(200).next_base = 300;
    target.space_level = 2;
    expect(must(ai::galactic_starbase_proposal(content, &target, 100)) == 300,
        "WGA-481 authored next type below the next-buildable level is still admitted");
    target.space_level = 1;
    target.maximum_space_level = 1;
    expect(must(ai::galactic_starbase_proposal(content, &target, 100)) == 100,
        "WGA-481 capped next-base lookup keeps the authored proposal");
    target.maximum_space_level = 5; gc::ProductionEntry queued; queued.type = 300; target.queues[1].push_back(queued);
    expect(must(ai::galactic_starbase_proposal(content, &target, 100)) == 100,
        "WGA-481 any queued station suppresses next-base substitution");
    target.queues[1].clear(); target.starbase_type = 300; target.space_level = 2;
    expect(must(ai::galactic_starbase_proposal(content, &target, 100)) == 100,
        "WGA-481 absent authored next link keeps the proposal");
    target.starbase_type = 0; target.space_level = 0; target.starbase_owner = 0; target.target_owner = 2;
    expect(must(ai::galactic_starbase_proposal(content, &target, 100)) == 400,
        "WGA-481 initial station resolution uses the target controller's faction root");
    target.target_owner = 3;
    expect(must(ai::galactic_starbase_proposal(content, &target, 100)) == 100,
        "WGA-481 absent initial-faction lookup preserves the original marker");
    expect(must(ai::galactic_starbase_proposal(content, nullptr, 100)) == 100,
        "starbase proposal without a target stays unchanged");
}
std::string garrison_fact_contracts() {
    ai::GalacticSelectionContent content;
    ai::GalacticSelectionContentType carrier; carrier.name = "CARRIER";
    auto original = std::make_shared<eawr::units::GalacticType>(); original->name = "CARRIER";
    original->starting_spawned_units[1] = {{"Alpha", 2}, {"BETA", 1}, {"ALPHA", 9}};
    carrier.original = original; carrier.space_escorts = {"BETA", "Alpha"}; content.types.emplace(100, carrier);
    for (const auto& [id, name, channel, power, movement] : {
        std::tuple{101ULL, "ALPHA", gc::ProductionDomain::ground, 100, "WALK"},
        std::tuple{102ULL, "BETA", gc::ProductionDomain::space, 20, "FLY"}}) {
        ai::GalacticSelectionContentType type; type.name = name; type.value.channel = channel; type.movement_class = movement;
        type.company = id == 102; // Deliberately differs from ordinary build-domain metadata.
        content.types.emplace(id, type);
        foc::AiType traits; traits.type_id = id; traits.name = name; traits.combat_power = ai::real(power); content.ai.types.push_back(traits);
    }
    content.planets.emplace(10, ai::GalacticSelectionContentPlanet{"MUD", 0, {}, {}});
    content.terrain["MUD"]["WALK"] = ai::real(1) / ai::real(2);
    const auto rows = must(ai::galactic_proposal_garrisons(content, 100, 3, 10));
    const std::array<gc::TypeId, 7> expected{101, 101, 102, 101, 101, 102, 101};
    expect(rows.size() == expected.size(), "proposal garrisons use first resolved-type count for duplicate entries, then authored escorts");
    std::string trace;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        expect(row.type == expected.at(index) && row.all_main_domains,
            "proposal garrison type order is retained and scoring admits either main domain");
        expect(row.channel == (row.type == 101 ? gc::ProductionDomain::space : gc::ProductionDomain::ground),
            "garrison unclassified fallback uses ground-company/base classification independently of ordinary build domain");
        expect(row.power == ai::real(row.type == 101 ? 100 : 20)
            && row.terrain_effectiveness == (row.type == 101 ? ai::real(1) / ai::real(2) : ai::real(1)),
            "garrison power and target terrain come from the spawned type rather than its carrier");
        trace += std::to_string(row.type) + ":" + std::to_string(row.power.repr) + ":" + std::to_string(row.terrain_effectiveness.repr) + ";";
    }
    expect(must(ai::galactic_proposal_garrisons(content, 100, 0, 10)).size() == 2, "empty lowest tech row retains only escorts");
    expect(!ai::galactic_proposal_garrisons(content, 100, 6, 10), "out-of-table tech refuses before indexing");
    expect(!ai::galactic_proposal_garrisons(content, 999, 3, 10), "missing original proposal type refuses");
    expect(!ai::galactic_proposal_garrisons(content, 100, 3, 999), "missing target terrain refuses");
    content.types.at(100).space_escorts.push_back("UNBOUND");
    expect(!ai::galactic_proposal_garrisons(content, 100, 3, 10), "unbound authored escort refuses the whole fact result");
    content.types.at(100).space_escorts.pop_back();
    original->starting_spawned_units[1].insert(original->starting_spawned_units[1].begin(), {"Missing", 5});
    const auto with_null = ai::galactic_proposal_garrisons(content, 100, 3, 10);
    expect(with_null && with_null.value().size() == rows.size(),
        "RGC-24: a starting-spawn name that resolves to no type is a null entry and adds nothing to the expansion");
    return trace;
}
std::shared_ptr<ai::GalacticSelectionContent> indexed_selection_content() {
    auto content = std::make_shared<ai::GalacticSelectionContent>();
    content->population_check = false; content->technology_weight = ai::real(1);
    content->faction_speed[1] = ai::real(1);
    content->planets.emplace(10, ai::GalacticSelectionContentPlanet{"MUD", 2, {}, {}});
    content->terrain["MUD"]["WALK"] = ai::real(1) / ai::real(2);
    for (gc::TypeId id = 1000; id < 1256; ++id) {
        ai::GalacticSelectionContentType type; type.name = "DUMMY_" + std::to_string(id);
        content->types.emplace(id, std::move(type));
        foc::AiType traits; traits.type_id = id; traits.name = "DUMMY";
        content->ai.types.push_back(std::move(traits));
    }
    for (const auto& [id, name, power, movement] : {
        std::tuple{98ULL, "alpha", 999, "FLY"}, std::tuple{99ULL, "ALPHA", 55, "WALK"},
        std::tuple{100ULL, "CARRIER", 7, "FLY"}, std::tuple{101ULL, "ALPHA", 100, "FLY"},
        std::tuple{102ULL, "BETA", 20, "FLY"}}) {
        ai::GalacticSelectionContentType type; type.name = name; type.movement_class = movement;
        type.company = id == 102; content->types.emplace(id, std::move(type));
        foc::AiType traits; traits.type_id = id; traits.name = name;
        traits.combat_power = ai::real(power); traits.category_bits = 1;
        content->ai.types.push_back(std::move(traits));
    }
    // Duplicate names retain the lowest content ID; duplicate AI identities
    // retain the first authored row, independently of that map order.
    for (const auto id : {99ULL, 100ULL}) {
        foc::AiType duplicate; duplicate.type_id = id;
        duplicate.combat_power = ai::real(999); duplicate.category_bits = 2;
        content->ai.types.push_back(std::move(duplicate));
    }
    auto original = std::make_shared<eawr::units::GalacticType>(); original->name = "CARRIER";
    original->starting_spawned_units[1] = {{"Alpha", 2}, {"BETA", 1}, {"ALPHA", 9}};
    auto& carrier = content->types.at(100); carrier.original = std::move(original);
    carrier.space_escorts = {"BETA", "Alpha"}; carrier.named_hero = carrier.value.hero = true;
    carrier.value.tech = 2; carrier.value.cost = ai::real(100); carrier.value.build_seconds = ai::real(1);
    return content;
}
std::string indexed_selection_contract(const eawr::sim::PartitionExecutor& executor) {
    auto session = must(gc::GalacticSession::create(setup()));
    Owner owner(session, executor);
    ai::GalacticPerceptionSetup perception;
    perception.players = {{1, 1, "Empire", "Normal", false, true, false, 10}};
    perception.planets = {{10, "ALPHA", 1000, {}, {}, {}, {}, {}}};
    perception.planets.front().enemy_indigenous.emplace();
    perception.types = owner.host->setup.content.types;
    auto world = std::make_shared<ai::GalacticWorld>(perception);
    ai::GalacticConnectivity connectivity; connectivity.goal_reach.push_back({1, 10, "ANY", {10}});
    auto published = session.snapshot();
    must(world->bind(published, connectivity, executor, 0, session.next_entity_id()));
    auto plan = definition(); plan.ignore_target = false; plan.allow_free_store = true;
    plan.galactic_contrast_qualified = true;
    auto plans = std::make_shared<ai::galactic_goals::MaterializedPlanCatalog>();
    plans->constraints[plan.name].forces.resize(plan.taskforces.size());
    const auto goal = data().goals.at("GOAL");
    auto content = indexed_selection_content();
    const auto expected = must(ai::galactic_proposal_garrisons(*content, 100, 2, 10));
    const auto expected_low_tech = must(ai::galactic_proposal_garrisons(*content, 100, 0, 10));
    expect(expected.size() == 7 && expected.front().type == 99 && expected.front().power == ai::real(55),
        "indexed fixture distinguishes canonical case, duplicate names, first AI row and first spawned count");
    const auto content_rows = content->types.size(), trait_rows = content->ai.types.size();
    const auto before = ai::galactic_selection_work();
    std::uint64_t freestore_reads{}, hero_reads{}; bool refuse_freestore{}, refuse_hero{};
    const auto heroes = [&](auto, auto, auto, const auto&) -> eawr::core::Result<ai::GalacticHeroSelection> {
        ++hero_reads;
        if (refuse_hero) {
            eawr::core::Diagnostic error; error.code = "TEST-HERO-LIVE"; error.message = "live hero refusal";
            return eawr::core::Result<ai::GalacticHeroSelection>::failure(std::move(error));
        }
        ai::GalacticHeroSelection result; result.target_weight = ai::real(static_cast<std::int64_t>(hero_reads));
        result.containment_allowed = true;
        return eawr::core::Result<ai::GalacticHeroSelection>::success(result);
    };
    const auto freestore = [&](auto, auto, const auto&) -> eawr::core::Result<std::vector<ai::GalacticSelectionObject>> {
        ++freestore_reads;
        if (refuse_freestore) {
            eawr::core::Diagnostic error; error.code = "TEST-FREESTORE-LIVE"; error.message = "live freestore refusal";
            return eawr::core::Result<std::vector<ai::GalacticSelectionObject>>::failure(std::move(error));
        }
        ai::GalacticSelectionObject row; row.object = 900 + freestore_reads; row.type = 100;
        return eawr::core::Result<std::vector<ai::GalacticSelectionObject>>::success({row});
    };
    std::weak_ptr<const ai::GalacticSelectionContent> lifetime = content;
    auto discarded = ai::stock_selection_provider(content, plans, world, owner.engine, [&] { return published; }, heroes, freestore);
    discarded = {}; // The runtime constructs a fresh provider for each read.
    auto provider = ai::stock_selection_provider(content, plans, world, owner.engine, [&] { return published; }, heroes, freestore);
    auto copied = provider;
    auto rebound = ai::stock_selection_provider(content, plans, world, owner.engine, [&] { return published; }, heroes, freestore);
    content.reset(); // The active bindings retain content independently of the weak cache.
    const auto equal_garrisons = [](const auto& actual, const auto& oracle) {
        return actual.size() == oracle.size() && std::equal(actual.begin(), actual.end(), oracle.begin(), [](const auto& a, const auto& b) {
            return a.type == b.type && a.channel == b.channel && a.power == b.power && a.population == b.population
                && a.terrain_effectiveness == b.terrain_effectiveness && a.all_main_domains == b.all_main_domains;
        });
    };
    const auto first = must(provider(1, 10, goal, plan));
    const auto second = must(rebound(1, 10, goal, plan));
    expect(first->complete && first->reachable && !first->producers.empty() && first->types.at(100).terrain_allowed,
        "stock indexed preparation uses the first AI row for target terrain and actual producer admission");
    expect(equal_garrisons(first->types.at(100).garrisons, expected)
        && equal_garrisons(second->types.at(100).garrisons, expected),
        "indexed provider and copied closure preserve the original garrison reader's full ordered values");
    expect(first->freestore.front().object == 901 && second->freestore.front().object == 902
        && first->types.at(100).hero_weight == ai::real(1) && second->types.at(100).hero_weight == ai::real(2),
        "immutable content preparation never memoizes live freestore or hero callbacks");
    auto changed = setup(); changed.player_economy->players.front().current_tech = 0;
    changed.production->planets.front().enabled = false;
    auto next = must(gc::GalacticSession::create(changed));
    (void)must(next.step(executor)); published = next.snapshot();
    must(world->bind(published, connectivity, executor, published->completed_tick(), next.next_entity_id()));
    const auto third = must(copied(1, 10, goal, plan));
    expect(third->complete && !third->reachable && third->producers.empty() && third->player_tech == 0
        && equal_garrisons(third->types.at(100).garrisons, expected_low_tech)
        && third->freestore.front().object == 903 && third->types.at(100).hero_weight == ai::real(3),
        "new completed publications retain their own tech, allowed sites, producer rows and live callback values");
    const auto after = ai::galactic_selection_work();
    expect(after.content_indices_built - before.content_indices_built == 1
        && after.content_index_rows - before.content_index_rows == content_rows
        && after.trait_index_rows - before.trait_index_rows == trait_rows,
        "content is indexed once across discarded, reconstructed and copied providers and new world publications");
    expect(after.garrison_name_lookups - before.garrison_name_lookups == 16
        && after.garrison_trait_lookups - before.garrison_trait_lookups == 16
        && after.proposal_trait_lookups - before.proposal_trait_lookups == 3,
        "lookup work follows emitted garrisons and proposal types rather than catalog rows times requests");
    expect(after.allowed_site_unowned == before.allowed_site_unowned, "the fixture planet is owned by the selecting player");
    expect(after.allowed_site_reads - before.allowed_site_reads == 2 && after.allowed_site_reuses - before.allowed_site_reuses == 1,
        "allowed-producer flags are read once per completed input and reused by a second provider on it; a new publication reads again");
    plans->constraints.at(plan.name).forces.front().deny_heroes = true;
    refuse_hero = true;
    const auto reads_before_denial = hero_reads;
    const auto denied_hero = must(provider(1, 10, goal, plan));
    expect(denied_hero->complete && !denied_hero->types.at(100).hero
        && denied_hero->types.at(100).hero_weight == ai::Real{} && hero_reads == reads_before_denial,
        "HA-01/HA-04 denied extra attach keeps an explicit named team ordinary without calling its failing hero provider");
    plans->constraints.at(plan.name).forces.front().deny_heroes = false;
    refuse_hero = false;
    refuse_freestore = true;
    const auto free_failure = provider(1, 10, goal, plan);
    expect(!free_failure && free_failure.error().code == "TEST-FREESTORE-LIVE", "new freestore refusal remains live");
    refuse_freestore = false; refuse_hero = true;
    const auto hero_failure = copied(1, 10, goal, plan);
    expect(!hero_failure && hero_failure.error().code == "TEST-HERO-LIVE", "new hero refusal remains live");
    refuse_hero = false;
    provider = {}; copied = {}; rebound = {};
    expect(lifetime.expired(), "the weak cache releases content with the last provider binding");
    auto malformed = indexed_selection_content(); malformed->types.at(100).space_escorts.push_back("MISSING");
    auto missing = ai::galactic_proposal_garrisons(*malformed, 100, 0, 10);
    auto bad = ai::stock_selection_provider(malformed, plans, world, owner.engine, [&] { return published; }, heroes, freestore);
    auto refused = bad(1, 10, goal, plan);
    expect(!missing && !refused && missing.error().code == refused.error().code && missing.error().message == refused.error().message,
        "indexed missing names refuse at the same authored entry as the original garrison reader");
    bad = {}; // End this immutable binding before preparing a different content owner.
    malformed = indexed_selection_content();
    std::erase_if(malformed->ai.types, [](const auto& row) { return row.type_id == 99; });
    missing = ai::galactic_proposal_garrisons(*malformed, 100, 0, 10);
    bad = ai::stock_selection_provider(malformed, plans, world, owner.engine, [&] { return published; }, heroes, freestore);
    refused = bad(1, 10, goal, plan);
    expect(!missing && !refused && missing.error().code == refused.error().code && missing.error().message == refused.error().message,
        "indexed missing garrison traits retain the original refusal and ordering");
    bad = {}; malformed = indexed_selection_content(); plan.ignore_target = true;
    std::erase_if(malformed->ai.types, [](const auto& row) { return row.type_id == 100; });
    bad = ai::stock_selection_provider(malformed, plans, world, owner.engine, [&] { return published; }, heroes, freestore);
    refused = bad(1, 10, goal, plan);
    expect(!refused && refused.error().message == "stock original AI type missing",
        "missing proposal traits still refuse when target contrast and garrisons are not requested");
    std::string trace;
    for (const auto& rows : {first->types.at(100).garrisons, third->types.at(100).garrisons})
        for (const auto& row : rows) trace += std::to_string(row.type) + ":" + std::to_string(row.power.repr) + ";";
    return trace + session.state_sha256() + next.state_sha256();
}
void staging_contracts() {
    ai::PlanDef plan; plan.taskforces.resize(3);
    std::vector<ai::galactic_goals::GalacticForceConstraints> constraints(3);
    constraints[1].unique_stage = constraints[2].unique_stage = true;
    const std::array<gc::PlanetId, 2> sites{10, 20};
    expect(must(ai::galactic_force_stages(plan, constraints, 30, "ANY", sites)) == std::vector<gc::PlanetId>{10, 20, 30},
        "unique staging excludes prior locations then falls back to the goal");
    constraints[1].unique_stage = false; constraints[1].matching_stage = true;
    expect(must(ai::galactic_force_stages(plan, constraints, 30, "ANY", sites)) == std::vector<gc::PlanetId>{10, 10, 20},
        "matching staging includes a prior location in authored order");
    expect(must(ai::galactic_force_stages(plan, constraints, 30, "SINGLE_HOP_DISCONNECTED", sites)) == std::vector<gc::PlanetId>(3, 30),
        "single-hop-disconnected stages directly at the goal");
    expect(must(ai::galactic_force_stages(plan, constraints, 0, "ANY", sites)) == std::vector<gc::PlanetId>(3), "global target does not invent staging");
    plan.magic = true;
    expect(must(ai::galactic_force_stages(plan, constraints, 30, "ANY", sites)) == std::vector<gc::PlanetId>(3), "magic plan leaves staging absent");
    plan.magic = false; plan.allow_free_store = false;
    expect(must(ai::galactic_force_stages(plan, constraints, 30, "ANY", sites)) == std::vector<gc::PlanetId>(3), "freestore-disabled plan leaves staging absent");
    expect(!ai::galactic_force_stages(plan, {}, 30, "ANY", sites), "missing authored stage constraints refuse");
}
void producer_admission_contracts() {
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        for (unsigned scenario = 0; scenario < 4; ++scenario) {
            auto configured = setup(); configured.player_economy->players.front().credits = {};
            if (scenario == 1) configured.production->players.front().locked = {100};
            if (scenario == 2) configured.production->planets.front().enabled = false;
            if (scenario == 3) {
                configured.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}};
                configured.fiscal->players.front() = {1, false, f(1)};
                configured.production->players.front().human = true;
                configured.production->players.front().pay_as_you_go = false;
            }
            auto session = must(gc::GalacticSession::create(configured));
            ai::GalacticPerceptionSetup perception;
            perception.players = {{1, 1, "Empire", "Normal", false, true, false, 10}};
            perception.planets = {{10, "ALPHA", 1000, {}, {}, {}, {}, {}}};
            foc::AiType type; type.type_id = 100; type.name = "UNIT"; type.tech_level = 2;
            perception.types = {type};
            ai::GalacticWorld world(perception);
            must(world.bind(session.snapshot(), {}, executor, 0, session.next_entity_id()));
            const auto input = world.copy_input();
            expect(input->producers.size() == 1, "producer admission cases retain the actual offer");
            const auto expected = scenario == 0 ? gc::ProductionAdmission::allowed
                : scenario == 1 ? gc::ProductionAdmission::locked
                : scenario == 2 ? gc::ProductionAdmission::unavailable : gc::ProductionAdmission::credits;
            expect(input->producers.front().admission == expected,
                "zero-credit AI retains eligible producer while locks, disabled sites and human credits still gate");
        }
    }
}
std::string sparse_force_work_contract(const eawr::sim::PartitionExecutor& executor) {
    const auto configured = gc::synthetic_galaxy({1, 32, 2, 8, 3, true, false});
    const auto session = must(gc::GalacticSession::create(configured));
    gc::NativeSnapshotChanges qualified; qualified.touched_planets.emplace();
    qualified.observer_generation = 0; // Disable the generation-only clock fast path so native revision work is exercised.
    auto actors = std::make_shared<std::vector<gc::CensusObject>>();
    for (const auto& fleet : configured.fleets) for (const auto& member : fleet.members) {
        gc::CensusObject actor; actor.id = member.id; actor.current_type = actor.original_type = member.type;
        actor.owner = fleet.owner; actor.planet = fleet.orbit; actor.container = fleet.id;
        actor.combat_role = gc::ActorCombatRole::ordinary; actors->push_back(actor);
    }
    std::sort(actors->begin(), actors->end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    qualified.actors = std::move(actors);
    const auto base = gc::GalacticSnapshot::native_view(session.snapshot(), std::move(qualified));
    ai::GalacticPerceptionSetup perception;
    perception.players = {{1, 1, "Empire", "Normal", false, true, false, 1},
        {2, 2, "Rebel", "Normal", false, true, false, 2}};
    perception.maximum_fleet_distance = ai::real(2000);
    perception.cache_intervals["ENEMYFORCE.SPACETOTALUNNORMALIZED"] = ai::real(-1);
    auto queries = data();
    queries.equations = must(ai::EquationSet::parse({{"force.xml", "<Equations>"
        "<Force>Variable_Target.EnemyForce.SpaceTotalUnnormalized</Force>"
        "<Nearby>Variable_Target.EnemyForce.NearbySpaceTotal</Nearby></Equations>"}}, {}));
    auto host = std::make_shared<foc::detail::Host>();
    ai::Engine engine(host, std::move(queries), {});
    for (const auto& planet : base->planets()) {
        ai::GalacticPlanetFacts facts; facts.planet = planet.id;
        facts.enemy_indigenous.emplace(); perception.planets.push_back(std::move(facts));
    }
    for (gc::TypeId id = 1000; id < 1003; ++id) {
        ai::GalacticForceType type; type.type = id; type.categories = 1; type.power = ai::real(7);
        perception.force_types.push_back(std::move(type));
    }
    ai::GalacticWorld sparse(perception), full(perception);
    full.enable_sparse_native_frontiers(false);
    must(sparse.bind(base, {}, executor, 0, session.next_entity_id()));
    must(full.bind(base, {}, executor, 0, session.next_entity_id()));
    gc::NativeSnapshotChanges moved;
    moved.fleets.emplace(base->fleets().begin(), base->fleets().end());
    auto& fleet = moved.fleets->front();
    const auto from = fleet.orbit;
    const auto to = from == 31 ? 29ULL : 31ULL;
    fleet.orbit = to;
    moved.touched_planets = std::vector<gc::PlanetId>{from, to};
    const auto first = gc::GalacticSnapshot::native_view(base, std::move(moved));
    gc::NativeSnapshotChanges empty; empty.touched_planets.emplace();
    const auto second = gc::GalacticSnapshot::native_view(first, std::move(empty));
    std::string trace;
    const auto compare = [&](const auto& snapshot, std::uint64_t expected_planets) {
        const auto before = sparse.native_frontier_work();
        must(sparse.bind_native_frontier(1, snapshot, executor, session.next_entity_id()));
        must(full.bind_native_frontier(1, snapshot, executor, session.next_entity_id()));
        const auto work = sparse.native_frontier_work();
        expect(work.prepared_force_planets - before.prepared_force_planets == expected_planets,
            "sparse force preparation visits exactly the changed planets, including a removed revision");
        expect(full.work().prepared_force_planets == 32, "full frontier oracle prepares all 32 force planets");
        for (const auto& planet : snapshot->planets()) {
            ai::Target target; target.object = planet.id;
            const ai::Context context{2, 1, {}, &target};
            const auto expected = full.evaluate(engine, "Force", context);
            const auto actual = sparse.evaluate(engine, "Force", context);
            expect(actual && actual == expected, "sparse force values equal the full oracle at every planet");
            expect(sparse.evaluate(engine, "Nearby", context) == full.evaluate(engine, "Nearby", context),
                "untouched planet neighbourhoods read changed neighbouring force rows");
            trace += std::to_string(actual->repr) + ":";
        }
        record_digest_contract(sparse, "sparse force query records retain correct warm/cold marks");
        expect(must(sparse.capture()).components == must(full.capture()).components,
            "full and sparse force reads retain identical perception archives");
    };
    compare(first, 2); compare(second, 0);
    gc::NativeSnapshotChanges reverted; reverted.touched_planets.emplace();
    compare(gc::GalacticSnapshot::native_view(base, std::move(reverted)), 2);
    gc::NativeSnapshotChanges unknown;
    compare(gc::GalacticSnapshot::native_view(base, std::move(unknown)), 32);
    class RefuseForcePhase final : public eawr::sim::PartitionExecutor {
    public:
        explicit RefuseForcePhase(const eawr::sim::PartitionExecutor& inner) : inner_(inner) {}
        std::size_t worker_count() const noexcept override { return inner_.worker_count(); }
        eawr::core::Result<void> execute(std::size_t count, const std::function<void(std::size_t)>& work) const override {
            return inner_.execute(count, work);
        }
        eawr::core::Result<void> execute_phase(std::string_view phase, std::size_t count,
            const std::function<void(std::size_t)>& work) const override {
            if (phase == "galactic-ai-forces") {
                eawr::core::Diagnostic error; error.code = "TEST-FORCE-PHASE"; error.message = "force staging refused";
                return eawr::core::Result<void>::failure(std::move(error));
            }
            return inner_.execute_phase(phase, count, work);
        }
    private:
        const eawr::sim::PartitionExecutor& inner_;
    } refused(executor);
    // Requalify the completed frontier before refusing sparse staging.
    gc::NativeSnapshotChanges unchanged; unchanged.touched_planets.emplace();
    const auto stable = gc::GalacticSnapshot::native_view(base, std::move(unchanged));
    must(sparse.bind(stable, {}, executor, 0, session.next_entity_id()));
    const auto old_view = sparse.view(); const auto old_input = sparse.copy_input();
    const auto old_state = must(sparse.capture());
    const auto refused_bind = sparse.bind_native_frontier(1, first, refused, session.next_entity_id());
    expect(!refused_bind && refused_bind.error().code == "TEST-FORCE-PHASE", "force phase refusal reaches sparse staging");
    expect(sparse.view() == old_view && sparse.copy_input() == old_input
        && must(sparse.capture()).components == old_state.components,
        "a refused sparse bind retains its prior publication and canonical records");
    ai::Target target; target.object = from;
    expect(sparse.evaluate(engine, "Force", ai::Context{2, 1, {}, &target})
        == full.evaluate(engine, "Force", ai::Context{2, 1, {}, &target}),
        "planet force rows remain readable after sparse staging refuses");
    must(sparse.bind_native_frontier(1, first, executor, session.next_entity_id()));
    ai::GalacticWorld retained(perception);
    gc::NativeSnapshotChanges physical_changes; physical_changes.touched_planets.emplace();
    physical_changes.production.emplace();
    for (const auto& location : configured.planets) {
        gc::PlanetProduction row; row.planet = location.id; row.target_owner = location.owner;
        physical_changes.production->planets.push_back(row);
    }
    physical_changes.observer_generation = 1;
    physical_changes.economy_generation = std::make_shared<const gc::EconomyInputGeneration>();
    auto physical = gc::GalacticSnapshot::native_view(base, std::move(physical_changes));
    must(retained.bind(physical, {}, executor, 0, session.next_entity_id()));
    const std::weak_ptr<const gc::GalacticSnapshot> physical_owner = physical;
    gc::NativeSnapshotChanges clocks; clocks.touched_planets.emplace();
    clocks.production = *physical->production();
    clocks.observer_generation = physical->observer_input_generation();
    clocks.economy_generation = physical->economy_input_generation();
    const auto clock_only = gc::GalacticSnapshot::native_view(physical, std::move(clocks));
    must(retained.bind(clock_only, {}, executor, 0, session.next_entity_id()));
    expect(retained.work().prepared_planets == 0, "ownership witness takes the ordinary clock-only fast path");
    physical.reset();
    expect(!physical_owner.expired(), "clock-only publication retains its older borrowed producer owner");
    const auto refused_retained = retained.bind_native_frontier(1, first, refused, session.next_entity_id());
    expect(!refused_retained && refused_retained.error().code == "TEST-FORCE-PHASE" && !physical_owner.expired(),
        "a refused sparse bind preserves the older physical owner behind a newer clock-only publication");
    return trace;
}
std::string frontier_rebind_contract(const eawr::sim::PartitionExecutor& executor, bool restored) {
    auto configured = setup();
    configured.players.push_back({2, 2, 0}); configured.planets.push_back({20, {}, 2});
    auto account = configured.player_economy->players.front(); account.player = 2;
    configured.player_economy->players.push_back(account);
    auto fiscal = configured.fiscal->players.front(); fiscal.player = 2;
    configured.fiscal->players.push_back(fiscal); configured.fiscal->planets.push_back({20, {}});
    auto producer = configured.production->players.front(); producer.player = 2;
    configured.production->players.push_back(producer);
    auto planet = configured.production->planets.front(); planet.planet = 20;
    planet.target_owner = planet.starbase_owner = planet.ground_occupier = 2;
    configured.production->planets.push_back(planet);
    auto session = must(gc::GalacticSession::create(configured));
    (void)must(session.step(executor));
    ai::GalacticPerceptionSetup perception; perception.seed = 42;
    perception.players = {{1, 1, "Empire", "Normal", false, true, false, 10},
        {2, 2, "Rebel", "Normal", false, true, false, 20}};
    perception.planets = {{10, "ALPHA", 1000, {}, {}, {}, {}, {}}, {20, "BETA", 1000, {}, {}, {}, {}, {}}};
    foc::AiType type; type.type_id = 100; type.name = "UNIT"; perception.types = {type};
    auto world = std::make_unique<ai::GalacticWorld>(perception);
    const auto ordinary_bind = [&](const auto& snapshot) {
        must(world->bind(snapshot, {}, executor, snapshot->completed_tick(), session.next_entity_id()));
    };
    ordinary_bind(session.snapshot());
    must(world->bind_native_frontier(1, session.snapshot(), executor, session.next_entity_id()));
    const auto saved_session = must(session.save_checkpoint_state());
    const auto saved_session_hash = session.state_sha256();
    const auto saved_world = must(world->capture());
    if (restored) {
        session = must(gc::GalacticSession::create(configured));
        must(session.load_checkpoint_state(saved_session));
        expect(session.state_sha256() == saved_session_hash, "fresh native owner restores the exact completed session");
        world = std::make_unique<ai::GalacticWorld>(perception);
        ordinary_bind(session.snapshot()); must(world->restore(saved_world));
        expect(must(world->capture()).components == saved_world.components,
            "two-player frontier restore preserves canonical perception before continuation");
    }
    std::string trace;
    const auto read_frontier = [&](gc::PlayerId player, const auto& snapshot, bool changed = true) {
        const auto before = world->native_frontier_work().binds;
        must(world->bind_native_frontier(player, snapshot, executor, session.next_entity_id()));
        const auto count = world->native_frontier_work().binds;
        expect(count == before + (changed ? 1U : 0U),
            "only a repeated player/publication pair skips its frontier bind");
        must(world->bind_native_frontier(player, snapshot, executor, session.next_entity_id()));
        expect(world->native_frontier_work().binds == count,
            "repeated read of the same immutable player publication does no duplicate work");
        expect(world->observer_input_generation() == snapshot->observer_input_generation(),
            "alternating player movement reads bind the actual segment generation");
        for (const auto& expected : snapshot->player_economy()->players) {
            const auto* actual = world->account(expected.player);
            expect(actual && actual->credits == expected.credits,
                "frontier reads include preceding economic-only staged credits");
            std::uint64_t queued{};
            for (const auto& site : snapshot->production()->planets) for (const auto& queue : site.queues)
                for (const auto& entry : queue) if (entry.builder == expected.player && entry.type == 100) ++queued;
            expect(world->production_counts(expected.player, 100).queued_player == queued,
                "frontier reads include preceding production queues after an ordinary rebind");
            trace += std::to_string(queued) + ":";
        }
        trace += must(world->state_hash()) + ":" + std::to_string(changed) + ";";
    };
    std::vector<gc::GalacticSession::NativeProductionNotice> notices;
    session.set_native_production_handler([&](const auto& notice) {
        expect(notice.kind == gc::GalacticSession::NativeProductionNotice::Kind::marked
            && notice.player == 1 && notice.planet_owner == 1 && notice.build
            && notice.amount == f(100) && notice.price == f(100),
            "frontier queue witness retains its actual producer's mark and ownership");
        notices.push_back(notice);
        return eawr::core::Result<gc::GalacticSession::NativeProductionPayment>::success({true, notice.player, notice.amount});
    });
    const auto base = session.snapshot(); ordinary_bind(base);
    const std::array<gc::PlayerId, 2> players{1, 2};
    auto barrier = must(session.begin_native_barrier(players, gc::galactic_max_commands));
    must(barrier->select_player(1));
    (void)must(session.add_credits(1, f(7), f(1)));
    const auto first_credit = session.snapshot();
    expect(first_credit != base && base->observer_input_generation()
        && first_credit->observer_input_generation() == base->observer_input_generation()
        && first_credit->economy_input_generation() != base->economy_input_generation(),
        "credit-only native write changes the publication without changing observer generation");
    read_frontier(1, first_credit);
    (void)must(session.add_credits(1, f(3), f(1)));
    const auto credits = session.snapshot();
    expect(credits != first_credit && credits->observer_input_generation() == first_credit->observer_input_generation(),
        "second credit-only write cannot reuse the preceding publication's player key");
    read_frontier(1, credits);
    ordinary_bind(base); read_frontier(1, credits);
    ordinary_bind(credits); read_frontier(1, credits);
    const auto admitted = must(session.admit_native_production({1, 10, 100, f(100)}, executor));
    expect(admitted.admission == gc::ProductionAdmission::allowed && admitted.build,
        "two-player frontier witness queues actual paid native production");
    expect(notices.size() == 1 && notices.front().build == admitted.build,
        "queued native identity matches its retained production mark");
    const auto first_player = session.snapshot();
    expect(first_player->observer_input_generation() == credits->observer_input_generation(),
        "queued production retains observer generation while changing economic facts");
    ordinary_bind(base); read_frontier(1, first_player);
    must(barrier->select_player(2));
    (void)must(session.add_credits(2, f(11), f(1)));
    const auto second_player = session.snapshot();
    expect(second_player != first_player, "second player's native layer has its own inherited publication");
    read_frontier(2, second_player);
    read_frontier(1, first_player);
    read_frontier(2, second_player);
    read_frontier(2, second_player, false);
    must(barrier->begin_tail()); must(barrier->commit());
    ordinary_bind(session.snapshot());
    return trace + session.state_sha256() + must(world->state_hash());
}
}
int main() {
    try {
        {
            eawr::platform::ThreadWorkerAdapter executor(1);
            auto session = must(gc::GalacticSession::create(setup()));
            Owner owner(session, executor);
            auto missing = data();
            missing.function_sets["FUNCTIONS"] = {{"GOAL", "Positive"}, {"GOAL", "MissingEquation"}};
            ai::Engine engine(owner.host, std::move(missing), std::vector{definition()});
            const std::array selected{ga::PlayerSelection{1, "FIXTURE", "NORMAL", {}, {}}};
            const auto result = engine.begin_galactic_barrier(*owner.world, {}, owner.scripts,
                owner.sequence, selected, &executor);
            // #4937: a function set with an entry whose equation never loaded is not registered, as in retail (named by galactic_ai::unresolved_goal_equations), not a startup failure.
            expect(static_cast<bool>(result), "a missing selected goal equation skips its function set instead of refusing startup");
        }
        staging_contracts();
        goal_reach_index_contracts();
        shared_planet_list_contracts();
        starbase_proposal_contracts();
        const auto garrison_trace = garrison_fact_contracts();
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter executor(workers);
            std::array<std::string, 4> traces;
            must(executor.execute_phase("proposal-garrison-facts", traces.size(), [&](std::size_t index) { traces[index] = garrison_fact_contracts(); }));
            for (const auto& trace : traces) expect(trace == garrison_trace, "proposal garrison facts equal at 1/2/4/8 workers");
        }
        producer_admission_contracts();
        std::string baseline;
        std::array<std::string, 5> running_baselines;
        std::array<std::string, 3> native_identity_baselines;
        std::array<std::string, 24> assembly_baselines;
        std::string frontier_baseline;
        std::string sparse_force_baseline;
        std::string indexed_selection_baseline, named_team_baseline;
        for (const auto count : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter executor(count);
            const auto prepared_content_trace = indexed_selection_contract(executor);
            if (indexed_selection_baseline.empty()) indexed_selection_baseline = prepared_content_trace;
            else expect(prepared_content_trace == indexed_selection_baseline, "indexed selection preparation equals at 1/2/4/8 workers");
            const auto force_trace = sparse_force_work_contract(executor);
            if (sparse_force_baseline.empty()) sparse_force_baseline = force_trace;
            else expect(force_trace == sparse_force_baseline, "sparse/full force equality at 1/2/4/8 workers");
            const auto frontier = frontier_rebind_contract(executor, false);
            expect(frontier == frontier_rebind_contract(executor, true),
                "two-player economic frontier continuation equals straight and restored execution");
            if (frontier_baseline.empty()) frontier_baseline = frontier;
            else expect(frontier == frontier_baseline, "two-player frontier equality at 1/2/4/8 workers");
            for (unsigned scenario = 0; scenario < assembly_baselines.size(); ++scenario) {
                const auto trace = [&] {
                    try {
                        const auto trace = assembly_contract(executor, scenario);
                        if (scenario == 12 || scenario == 13) expect(trace == assembly_contract(executor, scenario, false),
                            "full and sparse native launch/split answers and restored continuation are identical");
                        return trace;
                    }
                    catch (const std::exception& error) {
                        throw std::runtime_error("assembly scenario=" + std::to_string(scenario)
                            + " workers=" + std::to_string(count) + ": " + error.what());
                    }
                }();
                if (assembly_baselines[scenario].empty()) assembly_baselines[scenario] = trace;
                else expect(assembly_baselines[scenario] == trace, "source0 assembly continuation equals on 1/2/4/8 workers");
            }
            for (unsigned launch_case = 0; launch_case < 3; ++launch_case) {
                const bool cross_planet = launch_case != 0;
                const bool partial = launch_case == 2;
                auto configured = setup();
                configured.players.push_back({2, 2, 0});
                configured.seats = {{{1}, 1, gc::SeatKind::ai, 0, true, 0}};
                auto neutral_account = configured.player_economy->players.front(); neutral_account.player = 2;
                configured.player_economy->players.push_back(neutral_account);
                configured.fiscal->players.push_back({2, false, f(1)});
                auto neutral_production = configured.production->players.front(); neutral_production.player = 2;
                configured.production->players.push_back(neutral_production);
                configured.fleet_control->types.front().transport = true;
                configured.control.emplace();
                configured.control->change_seconds = f(7);
                configured.control->players = {{1, false, true, 1}, {2, true, false, 2}};
                configured.control->relations = {{1, 1, true}, {1, 2, false}, {2, 1, false}, {2, 2, true}};
                configured.control->types = {{100, 0, false, true}};
                configured.control->max_ground_forces = 10;
                configured.fleets = {{30, 1, 10, {{31, 100}}}};
                gc::ControlPlanet surface; surface.planet = 10; surface.type = 1000;
                surface.surface_accessible = true; surface.ground_base = {1, 1};
                surface.landed = {{40, 100, 1}};
                if (!cross_planet) surface.landed.push_back({41, 100, 1});
                configured.control->planets.push_back(surface);
                if (cross_planet) {
                    configured.planets.push_back({20, {}, 1});
                    configured.fiscal->planets.push_back({20, {}});
                    auto producer = configured.production->planets.front(); producer.planet = 20;
                    configured.production->planets.push_back(std::move(producer));
                    surface.planet = 20; surface.landed = {{41, 100, 1}};
                    configured.control->planets.push_back(surface);
                    if (partial) {
                        configured.fleets.push_back({50, 2, 20, {{51, 100}}});
                        configured.control->planets.front().landed.push_back({42, 100, 1});
                    }
                }
                auto session = must(gc::GalacticSession::create(configured)); Owner launched(session, executor);
                if (cross_planet) {
                    const auto& control = *session.snapshot()->control();
                    const auto source = std::find_if(control.state.begin(), control.state.end(), [](const auto& row) { return row.planet == 20; });
                    expect(source != control.state.end() && source->blockaded == partial,
                        "later launch source has the actual expected native blockade state");
                    if (partial) {
                        const auto refused = must(session.launch_native_transport(1, 41, 0));
                        expect(!refused.admitted && refused.reason == gc::GalacticRejectReason::movement_blocked,
                            "later refusal is a genuine blockade with an unspecified local target");
                    }
                }
                must(launched.engine->allocate_galactic_goal(1, 0, launched.engine->target_of_object(10)->id));
                expect(ai::EngineContractAccess::selected_reservation(*launched.engine), "native launch fixture selects a real plan");
                auto& player = ai::EngineContractAccess::player(*launched.engine); player.next_function = player.functions.size();
                must(launched.engine->service_galactic_system(1, ga::Service::goals, launched.scripts, launched.sequence));
                ai::GoalStateContributor state(launched.engine, eawr::core::sha256_hex(std::span<const std::uint8_t>{}));
                must(state.capture());
                auto& force = ai::EngineContractAccess::force(*launched.engine);
                {
                    const auto write = launched.engine->native_record_scope();
                    launched.engine->native_record_changed(player.player, 21, force.id);
                    launched.engine->mark_native_state_changed();
                    force.units = {40, 41}; force.types = {100, 100};
                    force.galactic_fleet = 30;
                    force.sources = {0, 0}; force.producers = {0, 0}; force.pool_tokens = {0, 0};
                    if (partial) {
                        force.units.push_back(42); force.types.push_back(100);
                        force.sources.push_back(0); force.producers.push_back(0); force.pool_tokens.push_back(0);
                    }
                }
                record_digest_contract(state, "launch fixture publishes its retained roster before admission");
                launched.actions->bind_admission_gate([] { return false; });
                auth::ServiceReport report; auth::ScriptCommand command;
                command.issuer = launched.engine->plan(force.plan)->instance; command.sequence = 91; command.verb = ai::verb_ai;
                command.arguments = {auth::Value::text("galactic_launch"), auth::Value{auth::Handle{ai::handle_taskforce, force.id}}};
                const auto block_id = ai::block_id(command.issuer, command.sequence);
                report.commands.push_back(std::move(command)); must(launched.engine->after_service(report));
                const auto fleet = launched.actions->containing_fleet(1, 40);
                const auto second = launched.actions->containing_fleet(1, 41);
                const auto retained = partial ? fleet : second;
                auto& block = ai::EngineContractAccess::retained_block(*launched.engine, block_id);
                expect(fleet && *fleet >= launched.world->next_entity_id() && *fleet < session.next_entity_id()
                    && retained && *retained < session.next_entity_id() && force.galactic_fleet == *retained,
                    "native launch retains a real fleet allocated beyond the bound perception frontier"
                        " (partial=" + std::to_string(partial) + ", fleet=" + std::to_string(fleet.value_or(0))
                        + ", copied=" + std::to_string(launched.world->next_entity_id()) + ", live=" + std::to_string(session.next_entity_id()) + ")");
                expect(block.finished && block.galactic_actor == (partial ? 0 : *retained)
                    && (partial ? !second : second && second != fleet),
                    "WGA-486 unspecified local launches retain actual distinct fleets; WGA-227 preserves partial effects");
                const auto snapshot = session.snapshot();
                const auto first_row = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& row) { return row.id == *fleet; });
                expect(first_row != snapshot->fleets().end() && first_row->orbit == 10 && first_row->members.size() == 1,
                    "first transport launches at its own source planet");
                expect(launched.actions->containing_fleet(1, 31) == 30 && *fleet != 30,
                    "preexisting retained fleet is not imposed as a launch target");
                if (!partial) {
                    const auto second_row = std::find_if(snapshot->fleets().begin(), snapshot->fleets().end(), [&](const auto& row) { return row.id == *second; });
                    expect(second_row != snapshot->fleets().end() && second_row->orbit == (cross_planet ? 20U : 10U)
                        && second_row->members.size() == 1, "second transport uses its own parent without cross-planet gathering");
                } else {
                    expect(!launched.actions->containing_fleet(1, 42), "later refusal stops before an otherwise launchable third member");
                }
                must(state.capture()); // Before the next perception bind, including partial admission.
                ai::EngineContractAccess::force_fleet(*launched.engine, force, session.next_entity_id());
                record_digest_contract(state, "invalid fleet identity remains represented by native records");
                expect(!state.capture(), "TaskForce identity at the live unissued frontier still refuses");
                ai::EngineContractAccess::force_fleet(*launched.engine, force, *retained);
                ai::EngineContractAccess::block_actor(*launched.engine, block, session.next_entity_id());
                record_digest_contract(state, "invalid block actor remains represented by native records");
                expect(!state.capture(), "block identity at the live unissued frontier still refuses");
                ai::EngineContractAccess::block_actor(*launched.engine, block, partial ? 0 : *retained);
                const auto saved = must(state.capture()); must(state.restore(saved));
                const auto native_saved = must(launched.actions->capture()); must(launched.actions->restore(native_saved));
                expect(must(state.capture()).components == saved.components,
                    "same-frame native identities retain exact goal-state continuation before the next bind");
                expect(must(launched.actions->capture()).components == native_saved.components,
                    "per-parent launch native state round trips exactly");
                ga::ActionRequest continued; continued.kind = ga::ActionKind::launch; continued.player = 1;
                continued.object = *retained; continued.members = partial ? std::vector<eawr::sim::EntityId>{40, 41, 42}
                    : std::vector<eawr::sim::EntityId>{40, 41};
                const auto before_continuation = session.state_sha256();
                const auto repeated = must(launched.actions->admit(continued));
                expect(repeated.admitted == !partial && repeated.request == (partial ? 0 : *retained)
                    && session.state_sha256() == before_continuation,
                    "restored launch continuation preserves actual fleets and a later blockade without duplicate effects");
                const auto trace = session.state_sha256() + must(state.state_hash()) + must(launched.actions->state_hash());
                auto& identity_baseline = native_identity_baselines.at(launch_case);
                if (identity_baseline.empty()) identity_baseline = trace;
                else expect(trace == identity_baseline, "native launch identity continuation matches on 1/2/4/8 workers");
            }
            for (const auto quote : {75, 250}) {
                auto ground_session = must(gc::GalacticSession::create(setup())); Owner ground(ground_session, executor);
                auto& facts = ground.facts->types.at(100);
                facts.base = true; facts.channel = gc::ProductionDomain::ground;
                facts.build_seconds = ai::real(120);
                facts.instance_allowed = false; facts.instance_remaining = 0;
                facts.target_cost = ai::real(quote); facts.target_build_seconds = ai::real(1);
                ground.facts->producers.clear();
                must(ground.engine->allocate_galactic_goal(1, 0, ground.engine->target_of_object(10)->id));
                const auto selected = ai::EngineContractAccess::selected_reservation(*ground.engine);
                expect(selected == (quote == 75),
                    "WGA-482 ground base admission uses target quote despite raw cost, instance denial and zero structure slots");
                if (selected) {
                    const auto& potential = *ai::EngineContractAccess::player(*ground.engine).proposed.front().potential;
                    expect(potential.units == std::vector<gc::TypeId>{100} && potential.producers == std::vector<eawr::sim::EntityId>{10}
                        && potential.cost == ai::real(100),
                        "WGA-482 ground base uses the target producer and retains the separate raw roster cost");
                }
            }
            {
                auto preference_session = must(gc::GalacticSession::create(setup())); Owner preference(preference_session, executor);
                ai::EngineContractAccess::base_preference_fixture(*preference.engine);
                auto& base = preference.facts->types.at(100); base.base = true;
                base.build_seconds = ai::real(120); base.target_cost = ai::real(100); base.target_build_seconds = ai::real(1);
                auto mobile = base; mobile.base = false; mobile.build_seconds = ai::real(1);
                preference.facts->types.emplace(200, mobile);
                auto& producer = preference.facts->producers.front(); producer.type = 200; producer.build_seconds = f(2);
                must(preference.engine->allocate_galactic_goal(1, 0, preference.engine->target_of_object(10)->id));
                expect(ai::EngineContractAccess::selected_reservation(*preference.engine)
                    && ai::EngineContractAccess::player(*preference.engine).proposed.front().potential->units == std::vector<gc::TypeId>{100},
                    "WGA-482 negative raw base time wins over a mobile candidate with positive excess despite appearing second");
            }
            {
                // HA-01/HA-04: an explicit named team stays ordinary when extra hero attach
                // is denied. Poisoned attach-only facts cannot veto it or change its RNG/state.
                for (const auto stage : {false, true}) {
                    auto hero_session = must(gc::GalacticSession::create(setup())); Owner hero(hero_session, executor);
                    auto& facts = hero.facts->types.at(100);
                    facts.hero = stage; facts.hero_containment_allowed = false;
                    facts.hero_weight = ai::real(-1000); facts.escort_weight = ai::real(-1000);
                    facts.hero_minimum_cost = ai::real(5000); facts.hero_maximum_cost = ai::real(1);
                    hero.facts->attachment_allowed = {{false, false}};
                    must(hero.engine->allocate_galactic_goal(1, 0, hero.engine->target_of_object(10)->id));
                    expect(ai::EngineContractAccess::selected_reservation(*hero.engine),
                        "HA-01/HA-04 explicit named hero teams ignore extra-attach denial and use neutral hero weight");
                    const auto& potential = *ai::EngineContractAccess::player(*hero.engine).proposed.front().potential;
                    expect(potential.units == std::vector<gc::TypeId>{100}, "explicit named team fills its required slot");
                    ai::GoalStateContributor saved(hero.engine, eawr::core::sha256_hex(std::span<const std::uint8_t>{}));
                    const auto trace = must(saved.state_hash()) + must(hero.actions->state_hash());
                    if (named_team_baseline.empty()) named_team_baseline = trace;
                    else expect(trace == named_team_baseline,
                        "denied attach is identical to ordinary selection, including RNG and complete native state on 1/2/4/8 workers");
                }
                {
                    auto allowed_session = must(gc::GalacticSession::create(setup())); Owner allowed(allowed_session, executor);
                    auto& facts = allowed.facts->types.at(100); facts.hero = true;
                    facts.hero_containment_allowed = false;
                    allowed.facts->attachment_allowed = {{true, false}};
                    must(allowed.engine->allocate_galactic_goal(1, 0, allowed.engine->target_of_object(10)->id));
                    expect(!ai::EngineContractAccess::selected_reservation(*allowed.engine),
                        "HA-03 allowed hero attachment still enforces the qualified hero admission gates");
                }
                {
                    auto configured = setup(); configured.fleets = {{30, 1, 10, {{31, 101}}}};
                    configured.fleet_control->types.push_back({101, false, true, false});
                    auto extra_session = must(gc::GalacticSession::create(configured)); Owner extra(extra_session, executor, true);
                    ai::EngineContractAccess::staging_fixture(*extra.engine);
                    auto extra_type = extra.facts->types.at(100); extra_type.hero = true;
                    extra_type.hero_containment_allowed = true; extra_type.hero_weight = ai::real(1000);
                    extra.facts->types.emplace(101, extra_type);
                    extra.facts->freestore.push_back({31, {}, false, 101, false});
                    extra.facts->attachment_allowed = {{false, false}};
                    expect(extra.host->view->find(31) && extra.host->view->find(31)->owner == 1,
                        "extra-attach regression supplies a live owned hero candidate");
                    must(extra.engine->allocate_galactic_goal(1, 0, extra.engine->target_of_object(10)->id));
                    expect(ai::EngineContractAccess::selected_reservation(*extra.engine)
                        && ai::EngineContractAccess::player(*extra.engine).proposed.front().potential->units == std::vector<gc::TypeId>{100},
                        "HA-01 denied extra hero does not join a team that never names it");
                }
                expect(foc::ai::galactic_hero_stage_applies(true, false, false, true)
                    && !foc::ai::galactic_hero_stage_applies(true, false, false, false)
                    && foc::ai::galactic_hero_stage_applies(true, true, false, false) && foc::ai::galactic_hero_stage_applies(true, false, true, false)
                    && !foc::ai::galactic_hero_stage_applies(false, false, false, true),
                    "WGA-489 a generic unscripted hero is an ordinary unit; named heroes, scripted heroes and special weapons keep their stage");
            }
            for (const auto two_teams : {false, true}) {
                auto base_session = must(gc::GalacticSession::create(setup())); Owner base(base_session, executor);
                ai::EngineContractAccess::base_fixture(*base.engine, two_teams);
                auto facts = base.facts->types.at(100); facts.base = true; facts.channel = gc::ProductionDomain::space;
                facts.target_cost = ai::real(100); facts.target_build_seconds = ai::real(1);
                base.facts->types.clear(); base.facts->types.emplace(200, facts);
                base.facts->proposal_substitutions = {{100, 200}, {101, 200}};
                base.facts->producers.clear(); // Base work must use the target quote, without a mobile producer.
                must(base.engine->allocate_galactic_goal(1, 0, base.engine->target_of_object(10)->id));
                expect(ai::EngineContractAccess::selected_reservation(*base.engine),
                    "WGA-481 substituted base is selected using target admission even with zero special-structure slots");
                const auto& selected_base = *ai::EngineContractAccess::player(*base.engine).proposed.front().potential;
                expect(selected_base.units == std::vector<gc::TypeId>(two_teams ? 2U : 1U, 200)
                    && std::all_of(selected_base.producers.begin(), selected_base.producers.end(), [](auto id) { return id == 10; }),
                    "WGA-481 retained roster and production use the resolved base identity and goal planet");
                if (two_teams) expect(std::set(selected_base.team_of_unit.begin(), selected_base.team_of_unit.end()).size() == 2,
                    "WGA-481 proposal dedup preserves both authored team bindings for colliding aliases");
            }
            for (const auto cost : {75, 125}) {
                auto session = must(gc::GalacticSession::create(setup())); Owner budget(session, executor);
                budget.facts->types.at(100).cost = ai::real(cost); budget.facts->producers.front().cost = f(cost);
                const auto identity = must(budget.engine->allocate_galactic_goal(1, 0, budget.engine->target_of_object(10)->id));
                expect(identity && ai::EngineContractAccess::selected_reservation(*budget.engine), "roster budget fixture selects and reserves production");
                const auto request = ai::EngineContractAccess::funding_request(*budget.engine);
                expect(request.cost == f(cost) && request.proposal_budget == f(cost)
                    && budget.funding->ledger(1)->goals.at(identity->goal).unallocated == Fixed{},
                    "WGA-21/345 fresh proposal budget is selected roster cost rather than category available cash");
                auto& player = ai::EngineContractAccess::player(*budget.engine); player.next_function = player.functions.size();
                must(budget.engine->service_galactic_system(1, ga::Service::goals, budget.scripts, budget.sequence));
                auto retained = budget.funding->state(); retained.at(1).goals.at(identity->goal).unallocated = f(25);
                must(budget.funding->restore(std::move(retained)));
                const auto running = ai::EngineContractAccess::funding_request(*budget.engine, true);
                expect(running.cost == f(cost) && running.unallocated_resources == f(25),
                    "WGA-21 preserves genuine positive retained unallocated resources in running feasibility");
            }
            for (const bool wrong_original : {false, true}) {
                auto configured = setup(); configured.fleets = {{30, 1, 10, {{20, 100}}}};
                auto session = must(gc::GalacticSession::create(configured)); Owner converted(session, executor);
                ai::EngineContractAccess::staging_fixture(*converted.engine);
                foc::AiType representation; representation.type_id = 101; representation.name = "REPRESENTATION";
                converted.host->setup.content.types.push_back(representation);
                converted.host->types.clear();
                for (const auto& type : converted.host->setup.content.types) converted.host->types.emplace(type.type_id, &type);
                auto view = std::make_shared<foc::detail::WorldView>(*converted.host->view);
                for (auto& unit : view->units) if (unit.id == 20) unit.type = 101;
                converted.host->view = view;
                auto& store = converted.actions->qualified_freestore();
                ga::FreestoreFacts actor; actor.object.id = 20; actor.object.type = 100; actor.current_type = 101;
                actor.object.owner = 1; actor.object.planet = 10; actor.object.kind = ga::FreestoreKind::unit;
                actor.parent = 30; actor.ai_usable = actor.qualified = true;
                must(store.update(actor)); const std::array<eawr::sim::EntityId, 1> offered{20}; must(store.initialize(1, offered));
                converted.actions->bind_freestore_validation([](const auto& decoded) {
                    const auto* retained = decoded.facts(20);
                    expect(retained && retained->object.owner == 1 && retained->object.type == 100
                        && retained->current_type == 101 && retained->parent == 30,
                        "converted fixture validates native original identity and current representation before assignment");
                    return eawr::core::Result<void>::success();
                });
                converted.facts->freestore = {{20, ai::real(1), false, 100, false}};
                const auto identity = must(converted.engine->allocate_galactic_goal(1, 0, converted.engine->target_of_object(10)->id));
                expect(identity && ai::EngineContractAccess::selected_reservation(*converted.engine), "converted actor admits a real selected roster");
                auto& player = ai::EngineContractAccess::player(*converted.engine);
                expect(player.proposed.front().potential->units == std::vector<gc::TypeId>{100}
                    && player.proposed.front().potential->sources == std::vector<std::uint8_t>{0}
                    && player.proposed.front().potential->freestore == std::vector<eawr::sim::EntityId>{20}
                    && player.reserved.at(20) == identity->goal,
                    "WGA-312 selects and reserves original pool type despite a different live representation");
                player.next_function = player.functions.size();
                must(converted.engine->service_galactic_system(1, ga::Service::goals, converted.scripts, converted.sequence));
                ai::EngineContractAccess::add_tasks(*converted.engine);
                player.tasks.front().source = 0; player.tasks.front().producer = 0;
                if (wrong_original) player.tasks.front().type = 101;
                must(converted.engine->service_galactic_system(1, ga::Service::execution, converted.scripts, converted.sequence));
                if (wrong_original) expect(player.tasks.front().finished && player.tasks.front().failed && !player.tasks.front().object
                    && store.contains(1, 20) && ai::EngineContractAccess::force(*converted.engine).units.empty(),
                    "WGA-312 current representation cannot satisfy a genuinely different original task type");
                else expect(player.tasks.front().finished && !player.tasks.front().failed && player.tasks.front().object == 20
                    && !store.contains(1, 20) && ai::EngineContractAccess::force(*converted.engine).units == std::vector<eawr::sim::EntityId>{20},
                    "WGA-312 source-zero execution assigns the reserved original type through the actual native owner");
            }
            for (unsigned scenario = 0; scenario < 4; ++scenario) {
                auto configured = setup(); configured.fleets = {{30, 1, 10, {{20, 100}, {21, 100}}}};
                auto session = must(gc::GalacticSession::create(configured)); Owner pending(session, executor);
                ai::EngineContractAccess::staging_fixture(*pending.engine);
                auto view = std::make_shared<foc::detail::WorldView>(*pending.host->view);
                for (auto& unit : view->units) if (unit.id == 20 || unit.id == 21) unit.type = 101;
                pending.host->view = view;
                auto& store = pending.actions->qualified_freestore();
                ga::FreestoreFacts actor; actor.object.id = 20; actor.object.type = 100; actor.current_type = 101;
                actor.object.owner = 1; actor.object.planet = 10; actor.object.kind = ga::FreestoreKind::unit;
                actor.parent = 30; actor.ai_usable = actor.qualified = true;
                must(store.update(actor)); auto fallback = actor; fallback.object.id = 21; must(store.update(fallback));
                const std::array<eawr::sim::EntityId, 2> offered{20, 21}; must(store.initialize(1, offered));
                pending.actions->bind_freestore_validation([](const auto& decoded) {
                    for (const auto& [id, facts] : decoded.inventory())
                        expect((id == 20 || id == 21) && facts.object.owner == 1 && facts.object.type == 100 && facts.parent == 30,
                            "transit fixture validates the actual native actor domain before enabling assignment");
                    return eawr::core::Result<void>::success();
                });
                pending.facts->freestore = {{20, ai::real(1), false, 100, false}, {21, ai::real(2), false, 100, false}};
                const auto identity = must(pending.engine->allocate_galactic_goal(1, 0, pending.engine->target_of_object(10)->id));
                expect(identity && ai::EngineContractAccess::selected_reservation(*pending.engine), "transit arbitration selects and reserves its actual native actor");
                auto& player = ai::EngineContractAccess::player(*pending.engine); player.next_function = player.functions.size();
                must(pending.engine->service_galactic_system(1, ga::Service::goals, pending.scripts, pending.sequence));
                ai::EngineContractAccess::add_tasks(*pending.engine);
                player.tasks.front().source = 0; player.tasks.front().producer = 0;
                const auto live_view = pending.host->view;
                actor.enroute = scenario != 3; must(store.update(actor)); pending.facts->freestore.front().in_transit = actor.enroute;
                if (scenario == 3) {
                    auto delayed = std::make_shared<foc::detail::WorldView>(*live_view);
                    std::erase_if(delayed->units, [](const auto& unit) { return unit.id == 20; }); pending.host->view = delayed;
                }
                if (scenario == 1 || scenario == 2) { store.release(1, identity->potential_plan); player.reserved.erase(20); }
                if (scenario == 2) {
                    fallback.enroute = true; must(store.update(fallback)); pending.facts->freestore.back().in_transit = true;
                }
                must(pending.engine->service_galactic_system(1, ga::Service::execution, pending.scripts, pending.sequence));
                if (scenario == 0 || scenario == 3) {
                    expect(!player.tasks.front().finished && !player.tasks.front().failed && !player.tasks.front().object
                        && store.contains(1, 20) && store.contains(1, 21),
                        "WGA-164/307 own reserved transit or pending view waits despite a ready unreserved fallback");
                    actor.enroute = false; must(store.update(actor)); pending.facts->freestore.front().in_transit = false;
                    pending.host->view = live_view;
                    must(pending.engine->service_galactic_system(1, ga::Service::execution, pending.scripts, pending.sequence));
                    expect(player.tasks.front().finished && !player.tasks.front().failed && player.tasks.front().object == 20
                        && !store.contains(1, 20) && store.contains(1, 21)
                        && ai::EngineContractAccess::force(*pending.engine).units == std::vector<eawr::sim::EntityId>{20},
                        "arrival assigns the exact reserved actor and leaves the fallback free");
                } else {
                    expect(player.tasks.front().finished && player.tasks.front().failed && !player.tasks.front().object
                        && store.contains(1, 20) && store.contains(1, 21),
                        "unreserved-only ready or transit rows still fail a nonnull requester instead of waiting forever");
                }
            }
            {
                auto refused_session = must(gc::GalacticSession::create(setup())); Owner refused(refused_session, executor);
                must(refused.engine->allocate_galactic_goal(1, 0, refused.engine->target_of_object(10)->id));
                expect(ai::EngineContractAccess::selected_reservation(*refused.engine), "fresh estimate refusal fixture first selects a real roster");
                const auto before = refused.funding->state(); refused.facts->reachable = false;
                expect(ai::EngineContractAccess::fresh_estimate_refused(*refused.engine) && refused.funding->state() == before,
                    "fresh rejected estimate refuses with a contextual diagnostic before Q24 conversion or any funding write");
            }
            for (unsigned scenario = 0; scenario < 5; ++scenario) {
                auto configured = setup();
                if (scenario == 3) {
                    configured.planets.push_back({20, {f(3), f(4), {}}, 1});
                    configured.fiscal->planets.push_back({20, {}});
                    auto producer = configured.production->planets.front(); producer.planet = 20;
                    configured.production->planets.push_back(std::move(producer));
                }
                auto running_session = must(gc::GalacticSession::create(configured)); Owner running(running_session, executor);
                const auto identity = must(running.engine->allocate_galactic_goal(1, 0, running.engine->target_of_object(10)->id));
                expect(identity && ai::EngineContractAccess::selected_reservation(*running.engine), "running budget fixture selects a paid roster");
                auto& p = ai::EngineContractAccess::player(*running.engine); p.next_function = p.functions.size();
                must(running.engine->service_galactic_system(1, ga::Service::goals, running.scripts, running.sequence));
                const auto plan = p.active.front().plan;
                expect(running.engine->active_goal_count(1, 1) == 1 && running.engine->active_goal_count(1, 999) == 0,
                    "active goal reader counts exact category and returns zero for absent bucket");
                const auto proposed_size = p.proposed.size(); p.proposed.push_back(p.active.front());
                const auto old_finished = p.active.front().finished; p.active.front().finished = true;
                expect(running.engine->active_goal_count(1, 1) == 1 && !running.engine->active_goal_count(999, 1),
                    "active membership includes retained finished rows, excludes proposals and refuses absent manager");
                p.active.front().finished = old_finished; p.proposed.resize(proposed_size);
                ai::EngineContractAccess::add_tasks(*running.engine);
                must(running.engine->service_galactic_system(1, ga::Service::execution, running.scripts, running.sequence));
                const auto build = p.tasks.front().completion;
                if (scenario == 3) {
                    auto state = running.funding->state(); state.at(1).categories.at(1).cash = f(150);
                    state.at(1).goals.at(identity->goal).unallocated = {};
                    must(running.funding->restore(std::move(state)));
                }
                if (scenario == 4) running.facts->reachable = false;
                ai::EngineContractAccess::running_maintenance(*running.engine, scenario);
                const auto ledger = running.funding->ledger(1);
                expect(p.active.size() == 1 && p.active.front().id == identity->goal && p.active.front().plan == plan,
                    "all category paths retain the same running plan without attaching or selecting again");
                expect(ledger->goals.at(identity->goal).reassessed && ledger->goals.at(identity->goal).capital == f(100)
                    && ledger->categories.at(1).available_cash == f(scenario == 3 ? 50 : 100),
                    "maintenance restores running paid capital once, with or without competing proposals");
                expect(p.exclusion_counts.at("EXCLUDED") == 1 && running.actions->purchases().size() == 1,
                    "running reassessment adds no exclusions or duplicate purchase");
                for (unsigned frame = 0; frame < 35; ++frame) (void)must(running_session.step(executor));
                const auto& completed = running.actions->purchases().at(build);
                expect(completed.completed && !completed.failed && completed.object && completed.billed > Fixed{},
                    "accepted running build pays and completes after a later goal maintenance");
                // WGEP-43/71: completion does not add a final installment.
                // Reconcile actual bills, including the retained residual mark.
                const auto outstanding = must(eawr::sim::math::subtract(completed.price, completed.billed));
                const auto settled = running.funding->ledger(1);
                expect(settled->goals.at(identity->goal).cost == outstanding && settled->goals.at(identity->goal).marked == outstanding
                    && settled->goals.at(identity->goal).capital == outstanding
                    && settled->categories.at(1).cash == must(eawr::sim::math::subtract(f(scenario == 3 ? 150 : 200), completed.billed))
                    && running_session.snapshot()->player_economy()->players.front().credits == must(eawr::sim::math::subtract(f(200), completed.billed)),
                    "actual producer bills reconcile retained goal/category cash and the world account without a final debt flush");
                ai::GoalStateContributor retained(running.engine, eawr::core::sha256_hex(std::span<const std::uint8_t>{}));
                const auto trace = running_session.state_sha256() + must(running.actions->state_hash()) + must(retained.state_hash())
                    + eawr::core::sha256_hex(must(ga::encode_funding_state(running.funding->state())));
                if (running_baselines[scenario].empty()) running_baselines[scenario] = trace;
                else expect(trace == running_baselines[scenario], "running maintenance, billing and completion hashes match on 1/2/4/8 workers");
                if (scenario == 0) {
                    // Fixture: remove a completed-work budget while its plan is retained.
                    const auto object = completed.object;
                    const auto credits = running_session.snapshot()->player_economy()->players.front().credits;
                    must(running.funding->remove_goal(1, identity->goal));
                    ai::EngineContractAccess::running_maintenance(*running.engine, 0);
                    expect(p.active.empty() && p.tasks.empty() && !running.engine->galactic_goal(1, identity->goal)
                        && p.exclusion_counts.at("EXCLUDED") == 0,
                        "running plan with no retained budget finishes through ordinary teardown");
                    expect(running.actions->purchases().size() == 1 && running.actions->purchases().at(build).object == object
                        && running_session.snapshot()->player_economy()->players.front().credits == credits,
                        "missing-budget teardown preserves completed production and grants no credits");
                }
            }
            for (const auto capital : {100, 30}) {
                auto paid_session = must(gc::GalacticSession::create(setup())); Owner paid(paid_session, executor);
                const auto identity = must(paid.engine->allocate_galactic_goal(1, 0, paid.engine->target_of_object(10)->id));
                expect(identity && ai::EngineContractAccess::selected_reservation(*paid.engine), "salvage fixture selects and funds actual paid roster");
                auto& p = ai::EngineContractAccess::player(*paid.engine); p.next_function = p.functions.size();
                must(paid.engine->service_galactic_system(1, ga::Service::goals, paid.scripts, paid.sequence));
                ai::EngineContractAccess::add_tasks(*paid.engine);
                must(paid.engine->service_galactic_system(1, ga::Service::execution, paid.scripts, paid.sequence));
                const auto build = p.tasks.front().completion;
                (void)must(paid_session.step(executor));
                const auto sampled = must(paid.actions->build_head(1, build));
                expect(sampled.progress == ai::Real{} && sampled.remaining_seconds < ai::real(1)
                    && must(paid.actions->build_remaining_cost(1, build)) == f(100)
                    && paid.funding->state().at(1).goals.at(identity->goal).remaining == f(1),
                    "clock-only advancement changes head countdown but not sampled paid progress, remaining price or fiscal budget time");
                auto state = paid.funding->state(); auto& ledger = state.at(1); auto& budget = ledger.goals.at(identity->goal);
                budget.capital = f(capital); ledger.categories.at(1).available_cash = f(200 - capital);
                must(paid.funding->restore(std::move(state)));
                ai::EngineContractAccess::pending_reservation(*paid.engine, identity->goal);
                p.active.front().finished = true;
                expect(!paid.engine->galactic_goal(1, identity->goal) && paid.engine->galactic_potential_for_goal(1, identity->goal) == identity->potential_plan
                    && paid.engine->galactic_reservation_valid(1, 999999, identity->potential_plan), "finished goal retains Potential and exact actor reservation lifetime");
                ai::EngineContractAccess::finish(*paid.engine, paid.scripts);
                const auto& purchase = paid.actions->purchases().at(build);
                const bool salvage = capital == 100;
                expect(purchase.task_finished && purchase.orphan && purchase.salvage == salvage
                    && paid_session.snapshot()->production()->planets.front().queues[0].empty() != salvage,
                    "actual plan teardown keeps selected salvage entry or cancels nonfitting entry after budget selection");
                expect(paid.funding->state().at(1).goals.empty() && paid.funding->state().at(1).dead_goals.empty()
                    && paid.funding->state().at(1).dead_budget == f(salvage ? 100 : 0)
                    && paid_session.snapshot()->player_economy()->players.front().credits == f(200), "salvage changes category/dead budget without granting player credits");
                const auto saved = must(paid.actions->capture()); must(paid.actions->restore(saved));
                expect(must(paid.actions->capture()).components == saved.components, "finished/salvage flags and independently retained actual entry round trip");
                for (unsigned frame = 0; frame < 35; ++frame) (void)must(paid_session.step(executor));
                const auto& settled = paid.actions->purchases().at(build);
                // WGA-159/160: subtract actual orphan installments, independently
                // of completion. Completion does not invent a final debt flush.
                const auto residual = must(eawr::sim::math::subtract(f(salvage ? 100 : 0), settled.billed));
                expect(paid.funding->state().at(1).dead_budget == residual
                    && paid_session.snapshot()->player_economy()->players.front().credits == must(eawr::sim::math::subtract(f(200), settled.billed))
                    && (salvage ? settled.completed && settled.object && settled.billed > Fixed{} : settled.failed && settled.billed == Fixed{}),
                    "actual orphan installments debit aggregate dead budget and credits; completion adds no unsourced residual settlement");
            }
            {
                auto orbital = setup(); orbital.production->planets.front().offers.front().domain = gc::ProductionDomain::space;
                orbital.seats = {{{1}, 1, gc::SeatKind::ai, 0, true, 0}}; // actual orbital completion requires its controller
                auto parent = must(gc::GalacticSession::create(orbital)); Owner built(parent, executor);
                built.facts->types.at(100).channel = gc::ProductionDomain::space;
                built.facts->producers.front().domain = gc::ProductionDomain::space;
                must(built.engine->allocate_galactic_goal(1, 0, built.engine->target_of_object(10)->id));
                expect(ai::EngineContractAccess::selected_reservation(*built.engine), "orbital fixture selects actual paid space production");
                auto& player = ai::EngineContractAccess::player(*built.engine); player.next_function = player.functions.size();
                must(built.engine->service_galactic_system(1, ga::Service::goals, built.scripts, built.sequence));
                ai::EngineContractAccess::add_tasks(*built.engine);
                must(built.engine->service_galactic_system(1, ga::Service::execution, built.scripts, built.sequence));
                const auto build = player.tasks.front().completion;
                for (unsigned frame = 0; frame < 35; ++frame) (void)must(parent.step(executor));
                must(built.world->bind(parent.snapshot(), {}, executor, parent.snapshot()->completed_tick(), parent.next_entity_id()));
                const std::array selected{ga::PlayerSelection{1, "FIXTURE", "NORMAL", {}, {}}};
                must(built.engine->begin_galactic_barrier(*built.world,
                    {parent.snapshot()->completed_tick(), parent.snapshot()->clock().galactic_frame}, built.scripts, built.sequence, selected, &executor));
                must(built.engine->service_galactic_system(1, ga::Service::execution, built.scripts, built.sequence));
                const auto& fleet = parent.snapshot()->fleets().front(); const auto& task = player.tasks.front();
                expect(task.finished && task.joined && task.object == fleet.members.front().id && task.object != fleet.id
                    && ai::EngineContractAccess::force(*built.engine).galactic_fleet == fleet.id
                    && must(built.actions->build_actor(1, build)) == task.object,
                    "actual orbital completion keeps outer fleet identity separate from assigned first direct member");
            }
            {
                auto configured = setup(); configured.planets.push_back({20, {f(3), f(4), {}}, 1});
                configured.fiscal->planets.push_back({20, {}}); // fiscal source tables cover every actual planet
                gc::PlanetProduction stage; stage.planet = 20;
                configured.production->planets.push_back(stage);
                configured.fleets = {{30, 1, 10, {{31, 100}}}};
                auto stage_session = must(gc::GalacticSession::create(configured)); Owner staged(stage_session, executor);
                ai::EngineContractAccess::stage_reader_fixture(*staged.engine);
                auto catalog = std::make_shared<ai::galactic_goals::MaterializedPlanCatalog>();
                catalog->plans = staged.engine->plans(); catalog->constraints["FIXTURE"].forces = {{}, {false, false, true, false, {}}};
                staged.engine->set_galactic_stage_catalog(catalog);
                ai::GalacticConnectivity paths; paths.execution_available = true;
                paths.execution_routes = {{1, 10, 20, false, ai::real(7), {}, {}}};
                paths.goal_reach = {{1, 20, "ANY", {10}}};
                must(staged.world->bind(stage_session.snapshot(), paths, executor, 0, stage_session.next_entity_id()));
                const auto input = staged.world->copy_input();
                expect(std::any_of(input->producers.begin(), input->producers.end(), [](const auto& producer) {
                    return producer.player == 1 && producer.planet == 10 && producer.type == 100
                        && producer.admission == gc::ProductionAdmission::allowed;
                }), "AI stage producer uses content eligibility independently of player payment admission");
                const std::array selected{ga::PlayerSelection{1, "FIXTURE", "NORMAL", {}, {}}};
                must(staged.engine->begin_galactic_barrier(*staged.world, {}, staged.scripts, staged.sequence, selected, &executor));
                must(staged.engine->allocate_galactic_goal(1, 0, staged.engine->target_of_object(20)->id));
                ga::FreestoreFacts actor; actor.object = {31, 100, 1, 10, ga::FreestoreKind::unit, false, false, false, {}}; actor.current_type = 100; actor.parent = 30;
                actor.ai_usable = actor.qualified = true;
                must(staged.actions->qualified_freestore().update(actor));
                expect(!staged.engine->galactic_freestore_selection_facts(1, 20, 31, staged.engine->plans().front()), "native reader refuses missing per-object flag instead of inferring garrison from type/parent");
                bool usable = true, garrison = true, combat_skip = false;
                staged.engine->set_galactic_stage_garrison([&](auto, const auto&, auto) { return eawr::core::Result<bool>::success(garrison); });
                staged.engine->set_galactic_stage_combat_skip([&](auto, const auto&, auto) { return eawr::core::Result<bool>::success(combat_skip); });
                staged.engine->set_galactic_stage_usability([&](auto, const auto&, auto) { return eawr::core::Result<bool>::success(usable); });
                must(staged.actions->qualified_freestore().update(actor));
                const auto pair = must(staged.engine->galactic_freestore_selection_facts(1, 20, 31, staged.engine->plans().front()));
                expect(pair.first == ai::real(37) && pair.second, "native stage reader averages helper squared plus actor-stage XY squared over all authored forces");
                {
                    // One (player, target, plan) batch resolves goal type and stages once; each read still answers live.
                    const auto read = staged.engine->galactic_freestore_selection_batch(1, 20, staged.engine->plans().front());
                    expect(must(read(31)) == pair && must(read(31)) == pair, "batch reader repeats the independent per-actor answer");
                    usable = false;
                    expect(must(read(31)).first == ai::galactic_goals::rejected_estimate(), "batch reader reads live source usability per actor");
                    usable = true; combat_skip = true;
                    expect(must(read(31)).first == ai::Real{}, "batch reader reads live combat skip per actor");
                    combat_skip = false;
                    expect(must(read(31)) == pair, "batch reader recovers with the live predicates");
                    expect(!read(32), "batch reader refuses an actor without retained facts like an independent read");
                    expect(!staged.engine->galactic_freestore_selection_batch(1, 20, staged.engine->plans().front())(32)
                        && !staged.engine->galactic_freestore_selection_facts(1, 20, 32, staged.engine->plans().front()),
                        "single-actor wrapper and batch refuse identically");
                }
                {
                    // The kept flags equal the direct producer read for every planet and player, owned or not.
                    for (const unsigned who : {1U, 2U}) {
                        const auto player_id = static_cast<gc::PlayerId>(who);
                        const ai::AllowedProductionSites sites(input, player_id);
                        for (std::size_t index = 0; index < input->targets.size(); ++index) {
                            const auto kept = sites.at(index);
                            expect(kept && kept.value() == ai::allowed_production_site(*input, player_id, input->targets[index].planet),
                                "kept allowed-producer flag equals the direct producer read");
                        }
                    }
                }
                {
                    // A goal proposed inside the pass is seen by the next read: no goal targets planet 10 yet, so the
                    // stage type is unresolved. The allowed-producer flags of the same completed input stay kept.
                    const auto targets = staged.world->copy_input()->targets.size();
                    const auto kept = ai::galactic_selection_work();
                    expect(!staged.engine->galactic_freestore_selection_facts(1, 10, 31, staged.engine->plans().front()),
                        "stage type is unresolved before a goal of the plan targets the planet");
                    must(staged.engine->allocate_galactic_goal(1, 0, staged.engine->target_of_object(10)->id));
                    expect(static_cast<bool>(staged.engine->galactic_freestore_selection_facts(1, 10, 31, staged.engine->plans().front())),
                        "a goal proposed inside the pass resolves the stage type at the next read");
                    const auto reused = ai::galactic_selection_work();
                    expect(reused.allowed_site_reads == kept.allowed_site_reads && reused.allowed_site_reuses - kept.allowed_site_reuses == targets,
                        "the goal change re-resolved the stage list from the kept allowed-producer flags");
                }
                paths.execution_routes.front().threat = ai::real(5);
                must(staged.world->bind(stage_session.snapshot(), paths, executor, 0, stage_session.next_entity_id()));
                const auto before_rebind = ai::galactic_selection_work();
                expect(must(staged.engine->galactic_freestore_selection_facts(1, 20, 31, staged.engine->plans().front())).first == ai::parse_real("12.5").value(), "failed helper still contributes XY when another stage helper succeeds");
                expect(ai::galactic_selection_work().allowed_site_reads - before_rebind.allowed_site_reads == staged.world->copy_input()->targets.size(),
                    "a rebound world publishes a new input, so its allowed-producer flags are read again");
                usable = false; must(staged.actions->qualified_freestore().update(actor));
                expect(must(staged.engine->galactic_freestore_selection_facts(1, 20, 31, staged.engine->plans().front())).first == ai::galactic_goals::rejected_estimate(), "unusable source rejects actor without a reachable helper");
                combat_skip = true; must(staged.actions->qualified_freestore().update(actor));
                expect(must(staged.engine->galactic_freestore_selection_facts(1, 20, 31, staged.engine->plans().front())).first == ai::Real{}, "native stealth and global proposals have zero stage cost");
                expect(must(staged.engine->galactic_freestore_selection_facts(1, 0, 31, staged.engine->plans().front())).first == ai::Real{}, "global proposal does not invent stage locations");
            }
            {
                auto layout = setup();
                gc::Fleet fleet; fleet.id = 30; fleet.owner = 1; fleet.orbit = 10; fleet.members = {{20, 100}, {21, 100}};
                layout.fleets.push_back(fleet);
                auto session = must(gc::GalacticSession::create(std::move(layout))); Owner owner(session, executor);
                const auto allocated = must(owner.engine->allocate_galactic_goal(1, 0, owner.engine->target_of_object(10)->id));
                expect(allocated.has_value(), "real galactic Engine allocates reservation identity for the native pool witness");
                auto& store = owner.actions->qualified_freestore();
                for (const auto id : {20ULL, 21ULL}) {
                    ga::FreestoreFacts facts; facts.object.id = id; facts.object.type = facts.current_type = 100;
                    facts.object.owner = 1; facts.object.planet = 10; facts.parent = 30;
                    facts.object.kind = ga::FreestoreKind::unit;
                    must(store.update(ga::ordinary_freestore_birth(facts)));
                }
                const std::array<eawr::sim::EntityId, 2> offers{20, 21}; must(store.initialize(1, offers));
                owner.actions->bind_freestore_validation([&](const auto& decoded) {
                    for (const auto& [id, facts] : decoded.inventory())
                        expect((id == 20 || id == 21) && facts.object.owner == 1 && facts.parent == 30,
                            "synthetic native pool uses the actual commissioned Engine actor domain");
                    return eawr::core::Result<void>::success();
                });
                must(owner.engine->reserve_galactic_object(1, allocated->goal, 20));
                must(owner.engine->reserve_galactic_object(1, allocated->goal, 21));
                expect(store.reservations(1) && store.reservations(1)->at(20) == allocated->potential_plan
                    && store.reservations(1)->at(21) == allocated->potential_plan
                    && must(owner.engine->galactic_execution_reservations(1)) == *store.reservations(1),
                    "actual Engine writes publish the same potential identity to retained pool reservations");
                const auto engine_before = owner.engine->galactic_execution_reservations(1).value();
                expect(!owner.engine->reserve_galactic_object(1, allocated->goal, 999999)
                    && must(owner.engine->galactic_execution_reservations(1)) == engine_before
                    && *store.reservations(1) == engine_before, "unknown actor refusal leaves both reservation owners unchanged");
                must(owner.engine->release_galactic_goal(1, allocated->goal));
                expect(store.reservations(1)->empty() && store.object_count(1) == 2
                    && must(owner.engine->galactic_execution_reservations(1)).empty(),
                    "actual Engine goal release clears its own holds without removing native pool actors");
            }
            {
                auto contrast_session = must(gc::GalacticSession::create(setup())); Owner contrast(contrast_session, executor);
                ai::EngineContractAccess::contrast_fixture(*contrast.engine, false);
                contrast.facts->contrast = {{gc::ProductionDomain::space, 1, ai::real(10), false}, {gc::ProductionDomain::space, 0, ai::real(10), false}};
                must(contrast.engine->allocate_galactic_goal(1, 0, contrast.engine->target_of_object(10)->id));
                expect(!ai::EngineContractAccess::selected_reservation(*contrast.engine), "zero-power parent cannot satisfy classified contrast alone");
                contrast.facts->types.at(100).garrisons = {{200, gc::ProductionDomain::space, ai::real(10), ai::real(1)}};
                expect(ai::EngineContractAccess::selected_reservation(*contrast.engine), "qualified garrison preference and chosen deficit reduction satisfy contrast");
                const auto& selected = *ai::EngineContractAccess::player(*contrast.engine).proposed.front().potential;
                expect(selected.current_contrast[0].force == ai::Real{} && selected.current_contrast[1].force == ai::Real{}, "garrison decreases best category and same-domain total exactly once");
            }
            {
                auto qualified_session = must(gc::GalacticSession::create(setup())); Owner qualified(qualified_session, executor);
                ai::EngineContractAccess::contrast_fixture(*qualified.engine, false);
                ai::EngineContractAccess::plan_contrast_fixture(*qualified.engine);
                qualified.facts->unscaled_domain_totals = true;
                qualified.facts->contrast = {{gc::ProductionDomain::ground, 0, ai::Real{}, false},
                    {gc::ProductionDomain::space, 0, ai::real(10), false},
                    {gc::ProductionDomain::ground, 1, ai::Real{}, false},
                    {gc::ProductionDomain::space, 1, ai::real(10), false}};
                qualified.facts->types.at(100).garrisons = {{200, gc::ProductionDomain::ground, ai::real(40), ai::real(1) / ai::real(2), true, ai::real(4)}};
                must(qualified.engine->allocate_galactic_goal(1, 0, qualified.engine->target_of_object(10)->id));
                qualified.facts->population_check = true; qualified.facts->remaining_population = ai::real(3);
                expect(!ai::EngineContractAccess::selected_reservation(*qualified.engine),
                    "qualified garrison uses its own population cost to reject an insufficient classified build capacity");
                qualified.facts->remaining_population = ai::real(4);
                expect(ai::EngineContractAccess::selected_reservation(*qualified.engine),
                    "plan-specific matrix and terrain-adjusted ground garrison satisfy the space main contrast");
                const auto& selected = *ai::EngineContractAccess::player(*qualified.engine).proposed.front().potential;
                expect(selected.target_contrast[1].force == ai::real(10) && selected.target_contrast[3].force == ai::real(20),
                    "production target totals stay unscaled while classified force receives the plan multiplier");
                expect(selected.current_contrast[3].force == ai::real(-20) && selected.current_contrast[1].force == ai::real(-30)
                    && selected.current_contrast[0].force == ai::Real{},
                    "garrison space reduction omits terrain and subtracts raw power from the matched domain total");
            }
            {
                auto fallback_session = must(gc::GalacticSession::create(setup())); Owner fallback(fallback_session, executor);
                ai::EngineContractAccess::contrast_fixture(*fallback.engine, false);
                ai::EngineContractAccess::plan_contrast_fixture(*fallback.engine);
                fallback.facts->unscaled_domain_totals = true;
                fallback.facts->contrast = {{gc::ProductionDomain::ground, 0, ai::Real{}, false},
                    {gc::ProductionDomain::space, 0, ai::real(16777219), false},
                    {gc::ProductionDomain::ground, 1, ai::Real{}, false},
                    {gc::ProductionDomain::space, 1, ai::Real{}, false}};
                fallback.facts->types.at(100).garrisons = {{200, gc::ProductionDomain::space, ai::real(2), ai::real(1) / ai::real(2), true}};
                must(fallback.engine->allocate_galactic_goal(1, 0, fallback.engine->target_of_object(10)->id));
                expect(ai::EngineContractAccess::selected_reservation(*fallback.engine), "unclassified qualified garrison preserves ordinary minimum-roster selection");
                const auto& selected = *ai::EngineContractAccess::player(*fallback.engine).proposed.front().potential;
                expect(selected.current_contrast[1].force == ai::real(16777217),
                    "garrison without a classified match reduces its inferred-domain total without binary32 residual rounding");
            }
            {
                auto zero_session = must(gc::GalacticSession::create(setup())); Owner zero(zero_session, executor);
                ai::EngineContractAccess::contrast_fixture(*zero.engine, true);
                zero.facts->contrast = {{gc::ProductionDomain::ground, 0, ai::real(10), false}};
                must(zero.engine->allocate_galactic_goal(1, 0, zero.engine->target_of_object(10)->id));
                expect(ai::EngineContractAccess::selected_reservation(*zero.engine), "eligible unclassified deficit admits zero contrast without an unmet team minimum");
                expect(ai::EngineContractAccess::player(*zero.engine).proposed.front().potential->units == std::vector<gc::TypeId>{100}, "unclassified fallback selects candidate with no minimum-roster requirement");
            }
            {
                auto limit_session = must(gc::GalacticSession::create(setup())); Owner limit(limit_session, executor);
                ai::EngineContractAccess::structure_fixture(*limit.engine);
                limit.facts->types.at(100).structure = true; limit.facts->types.at(100).instance_remaining = 1;
                limit.facts->remaining_structure_slots_by_channel[0] = 2;
                must(limit.engine->allocate_galactic_goal(1, 0, limit.engine->target_of_object(10)->id));
                expect(!ai::EngineContractAccess::selected_reservation(*limit.engine), "unqualified target quote refuses a targeted structure");
                limit.facts->types.at(100).target_cost = ai::real(250); limit.facts->types.at(100).target_build_seconds = ai::real(1);
                expect(!ai::EngineContractAccess::selected_reservation(*limit.engine), "target quote controls admission independently of raw roster cost");
                limit.facts->types.at(100).target_cost = ai::real(100);
                expect(ai::EngineContractAccess::selected_reservation(*limit.engine), "one allowed structure remains a valid selected roster");
                expect(ai::EngineContractAccess::player(*limit.engine).proposed.front().potential->units.size() == 1,
                    "per-type allowance one prevents a second structure despite two free channel slots");
            }
            {
                auto income_session = must(gc::GalacticSession::create(setup())); Owner income(income_session, executor);
                auto state = income.funding->state(); auto& category = state.at(1).categories.at(1);
                category.cash = category.available_cash = {}; category.income = category.available_income = f(200);
                must(income.funding->restore(std::move(state)));
                ai::EngineContractAccess::player(*income.engine).maintenance_due = false;
                must(income.engine->allocate_galactic_goal(1, 0, income.engine->target_of_object(10)->id));
                expect(ai::EngineContractAccess::selected_reservation(*income.engine),
                    "zero estimate supplements half-time income independently of maintenance eligibility");
                expect(income.funding->ledger(1)->goals.begin()->second.capital == Fixed{},
                    "half-time admission uses income without invented capital");
            }
            {
                auto session = must(gc::GalacticSession::create(setup())); Owner indexed(session, executor);
                auto view = std::make_shared<foc::detail::WorldView>(*indexed.host->view);
                for (eawr::sim::EntityId id = 20; id <= 26; ++id) {
                    foc::detail::ViewUnit unit; unit.id = id; unit.owner = id == 25 ? 2 : 1;
                    unit.type = id == 26 ? 101 : 100; view->units.push_back(unit);
                }
                indexed.host->view = view;
                for (const auto& [id, cost] : {std::pair{20ULL, 5}, {21ULL, 1}, {22ULL, 1},
                        {23ULL, 0}, {24ULL, 0}, {25ULL, 0}, {26ULL, 0}, {999ULL, 0}})
                    indexed.facts->freestore.push_back({id, ai::real(cost), false, id == 26 ? 101ULL : 100ULL, false});
                must(indexed.engine->allocate_galactic_goal(1, 0, indexed.engine->target_of_object(10)->id));
                const auto result = ai::EngineContractAccess::indexed_selection(*indexed.engine);
                expect(result.freestore == std::vector<eawr::sim::EntityId>{21, 22, 20, 0},
                    "typed cursors keep path/identity ties and skip reserved, assigned, foreign and absent actors");
                expect(result.sources == std::vector<std::uint8_t>{0, 0, 0, 2} && result.producers.back() == 10
                    && result.cost == ai::real(100), "incremental queue estimate includes only the chosen production subset");
            }
            auto combined = setup(); combined.planets.push_back({20, {f(12), {}, {}}, 1});
            combined.fiscal->planets.push_back({20, {}});
            combined.seats = {{{1}, 1, gc::SeatKind::ai, 0, true, 0}}; // actual native fleet commands require an active issuer
            auto second = combined.production->planets.front(); second.planet = 20; combined.production->planets.push_back(second);
            combined.rules.maximum_fleet_movement_distance = f(12);
            combined.fleets = {{30, 1, 10, {{31, 101}, {32, 101}}}};
            combined.fleet_control->types.push_back({101, false, true, false});
            combined.movement.emplace(); auto& movement = *combined.movement;
            movement.mode_modifier = f(1); movement.types = {{100, f(1), f(100), 0, true}, {101, f(1), f(10), 0, true}};
            movement.players = {{1, f(1), f(1), false}}; movement.planets = {{10, f(1), false}, {20, f(1), false}};
            movement.access = {{1, 10, true, false}, {1, 20, true, false}};
            // The real move creates another fleet record, so the actor table must
            // already qualify the existing fleet and both literal members.
            combined.census.emplace(); combined.census->types = {{100, {}}, {101, {}}};
            combined.census->players = {{1, 0, false, {}}};
            combined.census->objects = {
                {10, 100, 1, 10, 0, false, false, 100, 0, gc::ActorCombatRole::ordinary},
                {20, 100, 1, 20, 0, false, false, 100, 0, gc::ActorCombatRole::ordinary},
                {30, 100, 1, 10, 10, false, false, 100, 0, gc::ActorCombatRole::fleet},
                {31, 101, 1, 10, 30, false, false, 101, 0, gc::ActorCombatRole::ordinary},
                {32, 101, 1, 10, 30, false, false, 101, 0, gc::ActorCombatRole::ordinary}};
            auto session = must(gc::GalacticSession::create(combined)); Owner owner(session, executor);
            ai::EngineContractAccess::staging_fixture(*owner.engine); owner.facts->force_stages = {10};
            const auto identity = must(owner.engine->allocate_galactic_goal(1, 0, owner.engine->target_of_object(10)->id));
            expect(identity.has_value(), "authored positive fixture allocates a retained goal");
            owner.facts->complete = false;
            expect(!ai::EngineContractAccess::selected_reservation(*owner.engine), "incomplete provider refuses selection and reservation");
            owner.facts->complete = true;
            expect(ai::EngineContractAccess::selected_reservation(*owner.engine), "live selection uses D estimator and C feasibility/reservation");
            auto& player = ai::EngineContractAccess::player(*owner.engine);
            expect(player.reserved_credits.empty() && session.snapshot()->player_economy()->players.front().credits == f(200), "reservation never prepays a tactical wallet");
            expect(owner.funding->ledger(1)->goals.at(identity->goal).capital == f(100), "C owns the actual reservation");
            ai::EngineContractAccess::pending_reservation(*owner.engine, identity->goal);
            const auto reservations = must(owner.engine->galactic_execution_reservations(1));
            expect(reservations == std::map<eawr::sim::EntityId, std::uint64_t>{{999999, identity->potential_plan}},
                "execution copy preserves pending-absent actor to exact potential plan identity");
            const auto digest = eawr::core::sha256_hex(std::span<const std::uint8_t>{});
            ai::GoalStateContributor codec(owner.engine, digest); const auto saved = must(codec.capture());
            must(codec.restore(saved)); owner.engine->set_galactic_funding(owner.funding);
            expect(must(codec.capture()).components == saved.components, "mid-reservation native restore is byte identical");
            expect(must(owner.engine->galactic_execution_reservations(1)) == reservations,
                "execution reservation copy survives native restore without actor remapping");
            const std::array restored_selection{ga::PlayerSelection{1, "FIXTURE", "NORMAL", {}, {}}};
            must(owner.engine->begin_galactic_barrier(*owner.world, {}, owner.scripts, owner.sequence, restored_selection, &executor));
            auto& restored = ai::EngineContractAccess::player(*owner.engine); restored.next_function = restored.functions.size();
            expect(restored.exclusion_counts.at("EXCLUDED") == 1, "global exclusion registered once");
            must(owner.engine->service_galactic_system(1, ga::Service::goals, owner.scripts, owner.sequence));
            expect(restored.active.size() == 1 && restored.active.front().plan != 0, "live completed pass attaches after successful reserve");
            expect(ai::EngineContractAccess::force(*owner.engine).galactic_stage == 10, "attachment retains the qualified force staging planet");
            const auto staged = must(codec.capture()); must(codec.restore(staged));
            expect(ai::EngineContractAccess::force(*owner.engine).galactic_stage == 10, "force staging planet round trips in the native codec");
            ai::EngineContractAccess::force_stage(*owner.engine, 999998);
            record_digest_contract(codec, "missing stage remains represented by native records");
            expect(!codec.capture(), "missing staging planet refuses canonical native capture");
            ai::EngineContractAccess::force_stage(*owner.engine, 10);
            must(owner.engine->begin_galactic_barrier(*owner.world, {}, owner.scripts, owner.sequence, restored_selection, &executor));
            ai::EngineContractAccess::add_tasks(*owner.engine);
            must(owner.engine->service_galactic_system(1, ga::Service::execution, owner.scripts, owner.sequence));
            auto& resumed = ai::EngineContractAccess::player(*owner.engine);
            const auto build = resumed.tasks.front().completion;
            expect(build != 0 && !resumed.tasks.front().finished && session.snapshot()->production()->planets.front().queues[0].front().build_id == build,
                "execution waits on real paid admission build ID");
            const auto head = must(owner.actions->build_head(1, build));
            expect(head.entry && head.entry->build_id == build && head.progress == ai::Real{} && head.remaining_seconds == ai::real(1), "actual head retains build, sampled progress and independent countdown");
            const auto rows = must(owner.engine->galactic_build_budgets(*owner.actions, *owner.funding));
            expect(rows->size() == 1 && rows->front().builder == 1 && rows->front().planet == 10 && rows->front().build == build
                && rows->front().type == 100 && rows->front().cost_to_completion == ai::real(100) && rows->front().spendable == ai::real(100), "live publication carries exact task/head builder and retained goal values");
            expect(must(owner.engine->galactic_build_remaining_cost(1, resumed.tasks.front())) == f(100), "task remaining price differs from goal-budget quote");
            const auto production_revision = owner.actions->build_state_revision();
            const auto cached_builds = ai::EngineContractAccess::build_cache(*owner.engine);
            ga::FreestoreFacts moving; moving.object = {31, 101, 1, 10, ga::FreestoreKind::unit, false, false, false, {}}; moving.current_type = 101;
            moving.parent = 30; moving.ai_usable = moving.qualified = true;
            must(owner.actions->qualified_freestore().update(moving));
            const std::array<eawr::sim::EntityId, 1> offers{31}; must(owner.actions->qualified_freestore().initialize(1, offers));
            owner.actions->bind_admission_gate([] { return false; });
            owner.actions->bind_movement([](auto, const gc::GalacticSnapshot& snapshot) {
                ga::NativeActions::MovementInput input; input.ai_player = true;
                for (const auto& planet : snapshot.planets()) { gc::PathPlanet access; access.id = planet.id;
                    access.relation = gc::PathRelation::allied; input.access.push_back(access); input.orbital_occupants[planet.id] = 0; }
                return eawr::core::Result<ga::NativeActions::MovementInput>::success(std::move(input));
            }, [] { return false; }, [&session](auto player, auto fleet) { return session.recheck_native_orbit(player, fleet); });
            owner.actions->bind_freestore_movement([](auto, const auto& facts, const auto&) {
                ga::NativeActions::FreestoreMoveInput input; input.actor_valid = input.source_ai_usable = true;
                input.grandparent = facts.object.planet;
                return eawr::core::Result<ga::NativeActions::FreestoreMoveInput>::success(input);
            });
            // Publish the real packed execution route before direct freestore
            // admission, as in the movement fixture (WGA-385..389).
            ga::ExecutionRouteInput route; route.player = 1; route.maximum_distance = ga::Number(12);
            for (const auto& planet : session.snapshot()->planets())
                route.planets.push_back({planet.id, planet.position, {}, {}, {}, true});
            auto route_build = must(ga::build_execution_routes(route, executor));
            auto route_cache = std::make_shared<ga::ExecutionRoutes>(); route_cache->available = true;
            route_cache->rows = std::move(route_build.rows); route_cache->paths = std::move(route_build.paths);
            owner.actions->bind_freestore_path([route_cache](gc::PlayerId player_id, const gc::GalacticSnapshot& snapshot,
                gc::PlanetId source, gc::PlanetId destination) {
                using Result = eawr::core::Result<ga::NativeActions::FreestorePathInput>;
                auto found = ga::cached_execution_path(*route_cache, player_id, source, destination);
                if (!found) return Result::failure(found.error());
                ga::NativeActions::FreestorePathInput input;
                input.planets = std::move(found.value().planets); input.threat = found.value().threat;
                input.next_occupant = gc::PlayerId{0};
                if (input.planets.size() > 1) for (const auto& fleet : snapshot.fleets())
                    if (fleet.orbit == input.planets[1] && !fleet.motion.transit) { input.next_occupant = fleet.owner; break; }
                return Result::success(std::move(input));
            }, [] { return false; });
            owner.actions->bind_freestore_validation([](const auto&) { return eawr::core::Result<void>::success(); });
            expect(must(owner.actions->admit_freestore_move(1, 31, 20)), "actual paid head and freestore move coexist before union codec restore");
            build_alloc_probe::calls.store(0, std::memory_order_relaxed);
            build_alloc_probe::armed.store(true, std::memory_order_relaxed);
            const auto unchanged = owner.engine->galactic_build_budgets(*owner.actions, *owner.funding);
            build_alloc_probe::armed.store(false, std::memory_order_relaxed);
            expect(unchanged && unchanged.value().get() == rows.get()
                && unchanged.value()->front().build == build
                && owner.actions->build_state_revision() == production_revision
                && ai::EngineContractAccess::build_cache(*owner.engine) == cached_builds
                && build_alloc_probe::calls.load(std::memory_order_relaxed) == 0,
                "freestore-only writes reuse immutable task storage with no rescan or allocation");
            const auto movement_request = owner.actions->movements().begin()->first;
            const auto associated = must(codec.capture()); const auto producer_saved = must(owner.actions->capture());
            must(codec.restore(associated)); owner.engine->set_galactic_funding(owner.funding); must(owner.actions->restore(producer_saved));
            expect(must(codec.capture()).components == associated.components && must(owner.actions->capture()).components == producer_saved.components,
                "actual associated task, head and producer references round trip before continuation");
            must(owner.engine->begin_galactic_barrier(*owner.world, {}, owner.scripts, owner.sequence, restored_selection, &executor));
            auto& continued = ai::EngineContractAccess::player(*owner.engine);
            expect(must(owner.engine->galactic_build_budgets(*owner.actions, *owner.funding))->front().build == build
                && owner.actions->movements().at(movement_request).freestore_actor == 31
                && owner.actions->qualified_freestore().facts(31)->tracked_movement,
                "union restore republishes actual paid head and retained freestore movement association together");
            for (unsigned frame = 0; frame < 35; ++frame) (void)must(session.step(executor));
            must(owner.world->bind(session.snapshot(), {}, executor, session.snapshot()->completed_tick(), session.next_entity_id()));
            must(owner.engine->begin_galactic_barrier(*owner.world, {session.snapshot()->completed_tick(), session.snapshot()->clock().galactic_frame},
                owner.scripts, owner.sequence, restored_selection, &executor));
            must(owner.engine->service_galactic_system(1, ga::Service::execution, owner.scripts, owner.sequence));
            const auto& task = continued.tasks.front();
            expect(task.finished && !task.failed && task.object != 0 && task.object != build, "exact completion actor differs from admitted build identity");
            expect(session.snapshot()->production()->planets.front().objects.front().id == task.object, "build observer resolves actual producer-created actor");
            ai::EngineContractAccess::force_stage(*owner.engine, task.object);
            record_digest_contract(codec, "nonplanet stage remains represented by native records");
            expect(!codec.capture(), "live nonplanet actor cannot stand in for a staging planet");
            ai::EngineContractAccess::force_stage(*owner.engine, 10);
            ai::EngineContractAccess::release_twice(*owner.engine);
            expect(continued.exclusion_counts.at("EXCLUDED") == 0, "repeated release unregisters exclusions once and retains zero row");
            expect(must(owner.engine->galactic_execution_reservations(1)).empty(), "release clears pending-absent execution reservation copy");
            const auto trace = session.state_sha256() + must(owner.actions->state_hash()) + must(codec.state_hash());
            if (baseline.empty()) baseline = trace; else expect(trace == baseline, "live selection/reservation/paid execution equality at 1/2/4/8");
        }
    } catch (const std::exception& error) { galactic_test::expect(false, error.what()); }
    return galactic_test::failures == 0 ? 0 : 1;
}
