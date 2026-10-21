#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <ratio>
#include <span>
#include <vector>
#include <iostream>
#include <cstddef>
#include <string_view>
#include <string>
#include <optional>
#include "../support/env.hpp"
#include "../support/result.hpp"
// Native ownership/restore infrastructure coverage, not a live opponent proof.
#include "galactic_support.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/sync_digest.hpp"
#include "eawr/core/record_digest.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "galactic_ai.hpp"
#include "galactic_ai_actions.hpp"
#include "galactic_ai_goal_state.hpp"
#include "galactic_ai_plans.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>
using eawr::testing::must;

namespace eawr::script::foc::ai {
// Larger synthetic continuation state, using the selected stock definitions.
// This stages codec workload only, never claims live plan/task admission.
struct EngineContractAccess {
    static std::size_t enlarge(Engine& e, tactical::PlayerId player) {
        const auto native_record_write = e.native_record_scope();
        e.native_records_replaced();
        auto& p = *e.player_ai(player);
        const auto prototype = p.proposed.front();
        const auto definition = prototype.potential->plan;
        // Supply a synthetic taskforce definition if this authored plan has none.
        if (e.plans_[definition].taskforces.empty()) e.plans_[definition].taskforces.emplace_back();
        for (unsigned i = 0; i < 32; ++i) {
            auto g = prototype; g.id = p.next_goal_id++;
            g.potential->id = *e.allocate_plan_id(p);
            Plan plan; plan.id = *e.allocate_plan_id(p); plan.instance = *e.allocate_instance_id(p);
            plan.definition = definition; plan.goal = g.id; plan.player = player; plan.target = g.target;
            TaskForce tf; tf.id = *e.allocate_taskforce_id(p); tf.plan = plan.id;
            plan.taskforces = {tf.id}; g.plan = plan.id; g.active = true;
            BuildTask task; task.taskforce = tf.id; task.source = 2;
            p.tasks.push_back(task); p.active.push_back(g);
            p.taskforces.emplace(tf.id, tf); p.plan_of_instance.emplace(plan.instance, plan.id);
            p.running.emplace(plan.id, plan);
        }
        e.mark_native_state_changed();

        e.rebuild_runtime_index();
        return p.tasks.size();
    }
};
}

namespace {
using namespace galactic_test;
namespace cp = eawr::campaign;
namespace ga = eawr::script::foc::galactic_ai;
namespace ai = eawr::script::foc::ai;
namespace auth = eawr::script::authoritative;
namespace foc = eawr::script::foc;
namespace core = eawr::core;
using eawr::sim::math::Fixed;

Fixed whole(std::int64_t value) { return must(Fixed::from_integer(value)); }
core::Result<void> refused(std::string message) {
    core::Diagnostic error; error.code = "test.native-refusal"; error.message = std::move(message);
    return core::Result<void>::failure(std::move(error));
}
constexpr std::array<std::string_view, 5> names{
    "galactic-ai-lifecycle", "GALACTIC_PERCEPTION", "galactic-goals", "galactic-execution", "galactic-production"};
struct Slot final : ga::TrackedStateContributor {
    std::string name;
    std::vector<std::uint8_t> bytes{1};
    std::vector<std::string>* order{};
    bool throws{}, fail_rollback{};
    bool refuse_capture{};
    bool tracked{true};
    std::optional<std::uint64_t> state_revision() const noexcept override {
        return tracked ? TrackedStateContributor::state_revision() : std::nullopt;
    }
    core::Result<ga::NativeState> capture() const override {
        if (refuse_capture) return core::Result<ga::NativeState>::failure(refused("capture refused").error());
        ga::NativeState state; state.components[name] = bytes;
        return core::Result<ga::NativeState>::success(std::move(state));
    }
    core::Result<void> restore(const ga::NativeState& state) override {
        mark_state_changed(); order->push_back(name); bytes = state.components.at(name);
        if (bytes.front() == 255) {
            if (throws) throw std::runtime_error("injected partial write");
            return refused("injected partial write");
        }
        if (fail_rollback) return refused("injected rollback refusal");
        return core::Result<void>::success();
    }
    core::Result<std::string> state_hash() const override {
        throw std::runtime_error("compositor must capture, not hash each contributor");
    }
};
struct Slots {
    std::vector<std::string> order;
    std::array<std::shared_ptr<Slot>, 5> owners;
    std::shared_ptr<ai::GalacticFunding> funding = std::make_shared<ai::GalacticFunding>();
    Slots() {
        for (std::size_t i = 0; i < owners.size(); ++i) {
            owners[i] = std::make_shared<Slot>(); owners[i]->name = names[i]; owners[i]->order = &order;
        }
        must(funding->initialize(1, std::array{ai::EvaluatedFundingTemplate{1, {{1, whole(1)}}, {}}}, whole(100)));
    }
    cp::GalacticAiContributors contributors() const { return {owners[0], owners[1], owners[2], funding, owners[3], owners[4], {}, {}, {}, {}}; }
};
std::string cold_native_sync(const cp::GalacticAiContributors& contributors) {
    ga::NativeDigests digests{{"funding", contributors.funding->record_sync_hash(true)}};
    const auto append = [&](std::string name, const std::shared_ptr<ga::StateContributor>& owner) {
        auto records = must(owner->record_sync_hash(true));
        if (records.empty()) records = core::sync_state_digest(must(owner->capture()).components.at(name));
        digests.emplace(std::move(name), std::move(records));
    };
    append("galactic-ai-lifecycle", contributors.lifecycle); append("GALACTIC_PERCEPTION", contributors.perception);
    append("galactic-goals", contributors.goals); append("galactic-execution", contributors.execution); append("galactic-production", contributors.production);
    for (const auto& [name, owner] : contributors.external) append(name, owner);
    return ga::compose_native_sync_digests(digests);
}
void record_sum_contracts() {
    expect(ga::native_sync_digest_version == 2, "record-sum format has its assigned sync version");
    expect(eawr::core::build_identity().ends_with("+gcsync2"), "TCP, lobby and match admission share the sync version identity");
    core::RecordDigest forward, reverse;
    for (std::uint64_t id = 1; id <= 64; ++id) {
        core::RecordDigest::Bytes bytes; core::RecordDigest::append(bytes, id); core::RecordDigest::append(bytes, id * 3);
        forward.set(static_cast<std::uint32_t>(id % 4), 1, id, bytes);
    }
    for (std::uint64_t id = 64; id; --id) {
        core::RecordDigest::Bytes bytes; core::RecordDigest::append(bytes, id); core::RecordDigest::append(bytes, id * 3);
        reverse.set(static_cast<std::uint32_t>(id % 4), 1, id + 1000, bytes);
    }
    expect(forward.shards() == reverse.shards() && forward.digest("test") == reverse.digest("test"), "record sums ignore local storage and insertion order");
    expect(forward.byte_size() == 64 * 16, "record byte accounting covers the complete keyed payloads");
    const auto original = forward.digest("test");
    const auto writes = forward.writes();
    forward.relocate(0, 1, 4, 900);
    expect(forward.digest("test") == original && forward.byte_size() == 64 * 16 && forward.writes() == writes,
        "storage compaction retains hashes, byte accounting and sums without encoding a record");
    forward.relocate(0, 1, 900, 4);
    core::RecordDigest::Bytes changed; core::RecordDigest::append(changed, 4); core::RecordDigest::append(changed, 13);
    forward.set(0, 1, 4, changed);
    expect(forward.digest("test") != original, "replacing one retained value subtracts its old record hash");
    changed.clear(); core::RecordDigest::append(changed, 4); core::RecordDigest::append(changed, 12); forward.set(0, 1, 4, changed);
    expect(forward.digest("test") == original, "reverting a record restores the exact sums");
    forward.erase(0, 1, 4); forward.set(0, 1, 700, changed);
    expect(forward.digest("test") == original, "erase and reinsert retain semantic identity independently of local slot");
    changed[0] = 8; forward.set(0, 1, 700, changed);
    expect(forward.digest("test") != original, "record semantic keys participate in both hash lanes");
}
// Independent byte-at-a-time XXH64 from the published specification: the direct-load fast path must agree for
// every tail shape (32-byte blocks, 8-byte and 4-byte steps, single bytes), every seed and any alignment.
std::uint64_t reference_xxh64(std::span<const std::uint8_t> bytes, std::uint64_t seed) {
    constexpr std::uint64_t p1 = 0x9e3779b185ebca87ULL, p2 = 0xc2b2ae3d27d4eb4fULL, p3 = 0x165667b19e3779f9ULL,
        p4 = 0x85ebca77c2b2ae63ULL, p5 = 0x27d4eb2f165667c5ULL;
    const auto load = [&](std::size_t at, unsigned count) {
        std::uint64_t word{};
        for (unsigned i = 0; i < count; ++i) word |= std::uint64_t(bytes[at + i]) << (8U * i);
        return word;
    };
    const auto round = [&](std::uint64_t acc, std::uint64_t word) { return std::rotl(acc + word * p2, 31) * p1; };
    std::size_t at{};
    std::uint64_t hash = seed + p5;
    if (bytes.size() >= 32) {
        std::uint64_t v1 = seed + p1 + p2, v2 = seed + p2, v3 = seed, v4 = seed - p1;
        for (; bytes.size() - at >= 32; at += 32) {
            v1 = round(v1, load(at, 8)); v2 = round(v2, load(at + 8, 8)); v3 = round(v3, load(at + 16, 8)); v4 = round(v4, load(at + 24, 8));
        }
        hash = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        for (const auto v : {v1, v2, v3, v4}) hash = (hash ^ round(0, v)) * p1 + p4;
    }
    hash += bytes.size();
    for (; bytes.size() - at >= 8; at += 8) hash = std::rotl(hash ^ round(0, load(at, 8)), 27) * p1 + p4;
    if (bytes.size() - at >= 4) { hash = std::rotl(hash ^ (load(at, 4) * p1), 23) * p2 + p3; at += 4; }
    for (; at < bytes.size(); ++at) hash = std::rotl(hash ^ (std::uint64_t(bytes[at]) * p5), 11) * p1;
    hash = (hash ^ (hash >> 33U)) * p2; hash = (hash ^ (hash >> 29U)) * p3;
    return hash ^ (hash >> 32U);
}
void fingerprint_matches_reference() {
    std::vector<std::uint8_t> data(160);
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    for (auto& byte : data) { state = state * 6364136223846793005ULL + 1442695040888963407ULL; byte = static_cast<std::uint8_t>(state >> 56U); }
    for (const auto seed : {std::uint64_t{0}, std::uint64_t{0x454157522d53594eULL}})
        for (std::size_t start = 0; start < 4; ++start)
            for (std::size_t length = 0; length + start <= 140; ++length) {
                const std::span<const std::uint8_t> view(data.data() + start, length);
                expect(core::sync_fingerprint(view, seed) == reference_xxh64(view, seed), "sync fingerprint equals the byte-wise reference");
            }
}
// Constants computed with the byte-at-a-time code at BASE c1360927, before the direct-load paths existed.
void pinned_base_constants() {
    std::vector<std::uint8_t> data(140);
    std::uint64_t state = 0x1234567890abcdefULL;
    for (auto& byte : data) { state = state * 6364136223846793005ULL + 1442695040888963407ULL; byte = static_cast<std::uint8_t>(state >> 56U); }
    constexpr std::size_t lengths[] = {0, 3, 7, 31, 33, 140};
    constexpr std::uint64_t plain[] = {0xef46db3751d8e999ULL, 0xc0ec5c9ac8663453ULL, 0x5bb1fc818ab57b9eULL, 0x671ac1978eb279b3ULL, 0x1e0de80b74a9ccdaULL, 0x20f5c6307a1120a4ULL};
    constexpr std::uint64_t seeded[] = {0x9aaf6902af288adbULL, 0x46963d53416269d9ULL, 0xbd4d213049bb0d1cULL, 0x18e8fcbac9d85448ULL, 0x6bee2ad4439cfc01ULL, 0x5c802cdc174cc834ULL};
    for (std::size_t i = 0; i < 6; ++i) {
        const std::span<const std::uint8_t> view(data.data(), lengths[i]);
        expect(core::sync_fingerprint(view, 0) == plain[i], "sync fingerprint matches the pinned BASE value (seed 0)");
        expect(core::sync_fingerprint(view, 0x454157522d53594eULL) == seeded[i], "sync fingerprint matches the pinned BASE value (seeded)");
    }
    core::RecordDigest::Bytes mixed;
    core::RecordDigest::append(mixed, 0x0123456789abcdefULL);
    mixed.push_back(0x5a); mixed.push_back(0xa5);
    core::RecordDigest::append(mixed, 7);
    for (const char c : std::string_view("route")) mixed.push_back(static_cast<std::uint8_t>(c));
    core::RecordDigest::append(mixed, 0xfedcba9876543210ULL);
    core::RecordDigest digest;
    digest.set(1, 2, 3, mixed);
    digest.set(2, 5, 9, std::span<const std::uint8_t>(data.data(), 33));
    expect(digest.digest("pin") == "dcca6e740042670285c8257c1f5b8bf1f5300807ac5a047c0413e62beee98bb0", "mixed word and byte record stream matches the pinned BASE digest");
}
void sync_contracts() {
    fingerprint_matches_reference();
    pinned_base_constants();
    expect(core::sync_fingerprint({}) == 0xef46db3751d8e999ULL, "XXH64 empty published algorithm vector");
    const std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
    expect(core::sync_fingerprint(abc) == 0x44bc2cf5ad770999ULL, "XXH64 short algorithm vector");
    Slots slots;
    auto state = must(cp::ComposedGalacticAiState::create(slots.contributors()));
    const auto saved = must(state->capture());
    const auto original = must(state->sync_hash());
    expect(original == cold_native_sync(slots.contributors()), "incremental sync includes every actual native component");
    const auto counts = state->sync_encodes();
    for (unsigned i = 0; i < 10; ++i) expect(must(state->sync_hash()) == original, "unchanged sync state is stable");
    expect(counts == state->sync_encodes(), "unchanged sync state serializes no native owners");
    for (const auto& owner : slots.owners) {
        owner->bytes[0] = 2; owner->mark_state_changed();
        expect(must(state->sync_hash()) != original, "sync detects actual native owner mutation");
        expect(must(state->sync_hash()) == cold_native_sync(slots.contributors()), "sync equals independent cold owner reconstruction after mutation");
        owner->bytes[0] = 1; owner->mark_state_changed();
        expect(must(state->sync_hash()) == original, "restored bytes give same sync digest with a different local epoch");
    }
    must(state->restore(saved));
    expect(must(state->sync_hash()) == original, "native save/restore retains sync state exactly");
    expect(must(state->state_hash()) == must(ga::native_state_hash(saved)), "full checkpoint digest is still canonical SHA");
}
void contracts() {
    Slots slots;
    auto state = must(cp::ComposedGalacticAiState::create(slots.contributors()));
    const auto saved = must(state->capture());
    expect(saved.components.size() == 6, "all native components captured once");
    const auto original = must(state->state_hash());
    expect(original == must(ga::native_state_hash(saved)), "composed digest equals independent canonical hash");
    for (unsigned i = 0; i < 10; ++i) expect(must(state->state_hash()) == original, "unchanged hash reused");
    expect(state->digest_generations() == 1, "unchanged requests perform one SHA256 generation");
    expect(state->owner_encodes() == std::array<std::uint64_t, 6>{1, 1, 1, 1, 1, 1},
        "unchanged requests encode no owners despite full verification");
    for (const auto& owner : slots.owners) {
        const auto before = state->owner_encodes();
        owner->mark_state_changed(); owner->bytes[0] = 2;
        expect(must(state->state_hash()) != original, "each native mutation invalidates digest");
        const auto after = state->owner_encodes();
        const auto index = static_cast<std::size_t>(std::find(slots.owners.begin(), slots.owners.end(), owner) - slots.owners.begin());
        const auto cache_index = index < 3 ? index : index + 1;
        for (std::size_t i = 0; i < before.size(); ++i) expect(after[i] == before[i] + (i == cache_index ? 1 : 0), "only changed owner encodes");
        owner->mark_state_changed(); owner->bytes[0] = 1;
        expect(must(state->state_hash()) == original, "reverted bytes restore original digest");
    }
    const auto generations = state->digest_generations();
    must(state->restore(saved));
    expect(must(state->state_hash()) == original && state->digest_generations() == generations,
        "identical restore does not rehash unchanged canonical state");
    expect(slots.order == std::vector<std::string>(names.begin(), names.end()), "native restore follows explicit dependency order");
    const auto funding_before = slots.funding->state();
    must(slots.funding->notify({galactic::GalacticSession::CreditNotification::Kind::windfall, 1, whole(101), whole(1), {}}));
    expect(must(state->state_hash()) != original, "real ledger mutation invalidates digest");
    must(state->restore(saved));
    expect(slots.funding->state() == funding_before && must(state->state_hash()) == original, "funding restored through owned codec");
    for (const auto& owner : slots.owners) {
        auto damaged = saved; damaged.components[owner->name][0] = 255;
        // Also change earlier dependencies and the real ledger before refusal.
        damaged.components[std::string(names[0])][0] = owner == slots.owners[0] ? 255 : 2;
        auto changed = funding_before; changed.at(1).categories.at(1).cash = whole(123);
        damaged.components["funding"] = must(ga::encode_funding_state(changed));
        expect(!state->restore(damaged), "rejecting partial native write refused");
        expect(must(ga::encode_native_state(must(state->capture()))) == must(ga::encode_native_state(saved)),
            "all dependencies roll back after refusal");
    }
    slots.owners.back()->throws = true;
    auto damaged = saved; damaged.components[std::string(names.back())][0] = 255;
    expect(!state->restore(damaged) && must(state->state_hash()) == original, "throwing consumer is rolled back");
    slots.owners.front()->tracked = false;
    slots.owners.front()->bytes[0] = 9;
    expect(must(state->state_hash()) != original, "untracked provider safely recomputes without a bump");
    slots.owners.front()->tracked = true;
    must(state->restore(saved));
    slots.owners.front()->mark_state_changed(); slots.owners.front()->bytes[0] = 3;
    slots.owners.back()->mark_state_changed(); slots.owners.back()->refuse_capture = true;
    expect(!state->state_hash(), "late capture refusal after early cache update fails closed");
    slots.owners.back()->refuse_capture = false;
    expect(must(state->state_hash()) == must(ga::native_state_hash(must(state->capture()))), "retry composes already updated earlier cache");
    must(state->restore(saved));
    damaged = saved; damaged.components.erase("funding");
    expect(!state->restore(damaged), "missing component rejected before publication");
    damaged = saved; damaged.components["unknown"] = {1};
    expect(!state->restore(damaged), "unknown component rejected before publication");
    damaged = saved; damaged.version = 2;
    expect(!state->restore(damaged), "unsupported native envelope refused");
    auto incomplete = slots.contributors(); incomplete.execution.reset();
    expect(!cp::ComposedGalacticAiState::create(incomplete), "missing provider cannot remove refusal");
    slots.owners.back()->fail_rollback = true;
    damaged = saved; damaged.components[std::string(names.back())][0] = 255;
    expect(!state->restore(damaged) && !state->capture() && !state->state_hash(), "rollback failure poisons native owner");
}

// This owner cannot service gameplay. Its only purpose here is to retain A's
// actual selected lifecycle codec at a prepared campaign boundary.
struct Unserviced final : ga::Services {
    core::Result<void> initialize(galactic::PlayerId) override { return refused("runtime not composed"); }
    core::Result<void> clear_equations(galactic::PlayerId) override { return refused("runtime not composed"); }
    core::Result<void> apply_funding_writes(galactic::PlayerId, const ga::TemplatePolicy&) override { return refused("runtime not composed"); }
    core::Result<void> apply_policy(galactic::PlayerId, const ga::TemplatePolicy&, ga::PolicyWrite) override { return refused("runtime not composed"); }
    core::Result<void> run(const ga::ServiceInput&, auth::ScriptScheduler&) override { return refused("runtime not composed"); }
};
struct RealOwners {
    std::shared_ptr<foc::detail::Host> host = std::make_shared<foc::detail::Host>();
    std::shared_ptr<ai::GalacticWorld> world;
    std::shared_ptr<ai::Engine> engine;
    std::shared_ptr<ai::GalacticFunding> funding = std::make_shared<ai::GalacticFunding>();
    std::shared_ptr<ga::ExecutionStateContributor> execution;
    std::shared_ptr<ga::NativeActions> production;
    std::shared_ptr<ga::Manager> lifecycle;
    std::shared_ptr<ai::GoalStateContributor> goals;
    RealOwners(galactic::GalacticSession& galaxy, ga::LifecycleSetup setup, ai::GalacticPerceptionSetup perception,
        std::vector<ai::PlanDef> plans, const eawr::sim::PartitionExecutor& executor) {
        auto data = must(ga::load_data(setup));
        for (const auto& plan : plans) setup.plans.push_back({plan.name, plan.goals});
        host->setup.seed = perception.seed;
        for (const auto& player : perception.players) {
            const auto selected = std::find_if(setup.players.begin(), setup.players.end(), [&](const auto& p) { return p.player == player.player; });
            host->setup.players.push_back({player.player, player.faction, player.neutral, selected != setup.players.end(),
                selected != setup.players.end() ? selected->player_type : "", player.human});
        }
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        world = std::make_shared<ai::GalacticWorld>(std::move(perception)); host->ai_world = world;
        engine = std::make_shared<ai::Engine>(host, data, std::move(plans)); host->engine = engine.get();
        // This prepared boundary has no configured execution runtime. Refuse
        // every retained execution row rather than accepting unresolved IDs.
        execution = std::make_shared<ga::ExecutionStateContributor>([](const ga::ExecutionRetainedState& state) {
            return state.empty() ? core::Result<void>::success() : refused("prepared boundary has no execution runtime identities");
        });
        must(world->bind(galaxy.snapshot(), {}, executor, galaxy.completed_tick(), galaxy.next_entity_id()));
        auto scripts = must(auth::ScriptScheduler::create({}, {})); std::uint64_t sequence{};
        must(engine->begin_galactic_barrier(*world, {galaxy.completed_tick(), galaxy.clock().galactic_frame}, scripts, sequence, setup.players, &executor));
        ga::Capabilities capabilities; capabilities.queries = std::make_shared<ai::GalacticQueries>(*engine, *world);
        capabilities.services = std::make_shared<Unserviced>();
        lifecycle = must(ga::Manager::create(setup, std::move(capabilities)));
        goals = std::make_shared<ai::GoalStateContributor>(engine, core::sha256_hex(std::span<const std::uint8_t>{}));
        std::map<galactic::PlayerId, Fixed> multipliers;
        if (galaxy.snapshot()->player_economy()) for (const auto& player : galaxy.snapshot()->player_economy()->players) {
            multipliers.emplace(player.player, whole(1));
            // Prepared boundary: no runtime template initialization claim.
            must(funding->initialize(player.player, {}, player.credits));
        }
        production = std::make_shared<ga::NativeActions>(galaxy, funding, ga::NativeActions::Placement{}, std::move(multipliers));
        production->bind_handlers();
    }
    cp::GalacticAiContributors contributors() const {
        return {lifecycle, world, goals, funding, execution, production, [manager = engine, bound = world](const auto& ledgers) {
            for (const auto& [player, ledger] : ledgers) {
                const auto players = bound->players();
                if (std::none_of(players.begin(), players.end(), [&](const auto& p) { return p.player_id == player; })) return refused("unknown ledger player");
                for (const auto& [id, goal] : ledger.goals) { static_cast<void>(goal); if (!manager->galactic_goal(player, id)) return refused("unresolved funding goal identity"); }
            }
            return core::Result<void>::success();
        }, {}, {}, {}};
    }
};
galactic::GalacticSetup fixture() {
    galactic::SyntheticGalaxyOptions options; options.players = 2; options.planets = 2; options.fleets_per_player = 0;
    auto setup = galactic::synthetic_galaxy(options);
    setup.player_economy = galactic::PlayerEconomySetup{{{1, whole(100), whole(1000), 0, 0}, {2, whole(100), whole(1000), 0, 0}}};
    return setup;
}
ga::LifecycleSetup fixture_lifecycle() {
    ga::LifecycleSetup setup;
    setup.xml_paths = {"data/xml/difficultyadjustments.xml"};
    setup.xml[setup.xml_paths.front()] = "<Difficulty_Adjustments/>";
    return setup;
}
ai::GalacticPerceptionSetup fixture_perception(const galactic::GalacticSetup& setup) {
    ai::GalacticPerceptionSetup perception;
    for (const auto& player : setup.players) perception.players.push_back({player.player_id, player.faction_id, "", "Normal", false, false, false, 0});
    for (const auto& planet : setup.planets) perception.planets.push_back({planet.id, "P" + std::to_string(planet.id), 0, {}, {}, {}, {}, {}});
    return perception;
}
void native_action_frontiers() {
    const eawr::sim::InlineExecutor executor;
    auto galaxy = must(galactic::GalacticSession::create(fixture()));
    RealOwners owners(galaxy, fixture_lifecycle(), fixture_perception(galaxy.setup()), {}, executor);
    auto& actions = *owners.production;
    const std::array players{galactic::PlayerId{1}, galactic::PlayerId{2}};
    const auto root = must(actions.state_hash());
    const auto revision = actions.state_revision();
    must(actions.prepare_player_frontiers(executor, players));
    {
        auto first = actions.player_frontier(1);
        must(actions.qualified_freestore().initialize(1, {}));
        const auto own = must(actions.state_hash());
        const auto own_revision = actions.state_revision();
        expect(own != root && own_revision != revision, "native private freestore writes own their physical epoch");
        {
            auto second = actions.player_frontier(2);
            expect(must(actions.state_hash()) == root && actions.state_revision() == revision,
                "native foreign freestore reads and epochs retain the frozen root");
            expect(!actions.qualified_freestore().initialized(1), "native foreign membership ignores earlier private initialization");
            must(actions.qualified_freestore().initialize(2, {}));
            expect(must(actions.record_sync_hash()) == must(actions.record_sync_hash(true)),
                "native private record sums retain the independent cold oracle");
        }
        expect(must(actions.state_hash()) == own && actions.state_revision() == own_revision,
            "nested native scopes restore both private state and its epoch");
    }
    expect(must(actions.state_hash()) == root && actions.state_revision() == revision,
        "native private writes leave the canonical state and epoch unchanged");
    must(actions.prepare_player_frontiers(executor, players));
    { auto first = actions.player_frontier(1); expect(must(actions.state_hash()) == root, "next native barrier resets physical frontiers"); }
    const std::array reversed{players[1], players[0]};
    expect(!actions.prepare_player_frontiers(executor, reversed), "native frontier preparation requires canonical owner order");
}
void real_boundaries() {
    std::vector<std::string> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = must(cp::GameSession::create(fixture()));
        if (workers > 1) game.set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
        std::shared_ptr<RealOwners> owners;
        auto state = must(cp::install_composed_galactic_ai(game, [&](auto& world) {
            expect(&world == &game.galaxy(), "factory receives actual retained GameSession world");
            owners = std::make_shared<RealOwners>(world, fixture_lifecycle(), fixture_perception(world.setup()), std::vector<ai::PlanDef>{}, executor);
            return core::Result<cp::GalacticAiContributors>::success(owners->contributors());
        }));
        const auto check_records = [&] {
            for (const auto& owner : std::array<std::shared_ptr<ga::StateContributor>, 5>{owners->lifecycle, owners->world, owners->goals, owners->execution, owners->production})
                expect(must(owner->record_sync_hash()) == must(owner->record_sync_hash(true)), "real owner writes equal independently rebuilt record sums");
            expect(owners->funding->record_sync_hash() == owners->funding->record_sync_hash(true), "funding writes equal independently rebuilt record sums");
        };
        check_records();
        auto duplicate = cp::install_composed_galactic_ai(game, owners->contributors());
        expect(!duplicate, "native owner cannot be installed twice");
        const auto prepared = must(state->capture());
        ga::ExecutionStateContributor unsupported;
        must(unsupported.replace({{1, {}}}));
        auto invalid_execution = prepared;
        invalid_execution.components["galactic-execution"] = must(unsupported.capture()).components.at("galactic-execution");
        expect(!state->restore(invalid_execution), "prepared boundary refuses unconfigured execution identities");
        expect(must(ga::encode_native_state(must(state->capture()))) == must(ga::encode_native_state(prepared)),
            "prepared execution refusal rolls all real owners back");
        std::vector<std::string> hashes;
        for (unsigned i = 0; i < 4; ++i) {
            const auto tick = must(game.step(executor)); hashes.push_back(tick.state_hash.get());
            expect(hashes.back() == game.state_sha256(), "native fast path matches independent boundary composition");
        }
        const auto identities = eawr::checkpoint::game_identities(fixture());
        const auto bytes = must(eawr::checkpoint::save_game_checkpoint(game, identities));
        std::shared_ptr<RealOwners> restored_owners;
        eawr::checkpoint::GameLoadOptions options;
        options.configure_native_ai = [&](auto& resumed) {
            auto installed = cp::install_composed_galactic_ai(resumed, [&](auto& world) {
                restored_owners = std::make_shared<RealOwners>(world, fixture_lifecycle(), fixture_perception(world.setup()), std::vector<ai::PlanDef>{}, executor);
                return core::Result<cp::GalacticAiContributors>::success(restored_owners->contributors());
            });
            return installed ? core::Result<void>::success() : core::Result<void>::failure(installed.error());
        };
        auto restored = must(eawr::checkpoint::load_game_checkpoint(bytes, identities, options, executor));
        expect(restored_owners && restored_owners.get() != owners.get(), "checkpoint creates fresh owners before ordered restore");
        expect(must(eawr::checkpoint::save_game_checkpoint(restored.session, identities)) == bytes, "six real contributors canonically recapture checkpoint");
        expect(restored.session.galaxy().cached_state_hash().get() == restored.session.galaxy().state_sha256(),
            "cached world hash immediately after checkpoint restore equals independent recompute");
        for (unsigned i = 0; i < 3; ++i) expect(must(game.step(executor)).state_hash.get() == must(restored.session.step(executor)).state_hash.get(), "real contributor continuation equals uninterrupted owner");
        if (reference.empty()) reference = hashes; else expect(reference == hashes, "real native boundary hashes equal on 1/2/4/8 workers");
        expect(state->digest_generations() == 1, "frozen native owners reuse digest across galaxy frames");
        expect(state->owner_encodes() == std::array<std::uint64_t, 6>{1, 1, 1, 1, 1, 1}, "all six real frozen owners tracked");
        const auto funding_revision = owners->funding->state_revision();
        const auto before_rows = must(state->state_hash());
        auto ledger = *owners->funding->ledger(1);
        ledger = must(ai::funding::fiscal(std::move(ledger), whole(100), whole(3), whole(1)));
        expect(ledger != *owners->funding->ledger(1), "funding publication fixture changes retained interval state");
        must(owners->funding->commit_rows({{1, std::move(ledger)}}, [] { return core::Result<void>::success(); }));
        expect(owners->funding->state_revision() != funding_revision, "immediate funding publication owns its revision bump");
        expect(must(state->state_hash()) != before_rows, "published funding rows invalidate the cached digest");
        expect(must(state->state_hash()) == must(ga::native_state_hash(must(state->capture()))), "published funding rows match full oracle");
        check_records();
        const auto hint_revision = owners->world->state_revision();
        expect(owners->world->insert_hint({1, "P1", "test", ai::real(3)}), "retained hint inserted");
        expect(owners->world->state_revision() != hint_revision, "perception mutation bumps its own epoch");
        expect(must(state->state_hash()) == must(ga::native_state_hash(must(state->capture()))), "real perception mutation matches full oracle");
        check_records();
        owners->world->set_story_flag(1, 1, "first"); owners->world->set_story_flag(2, 1, "second"); check_records();
        expect(owners->world->check_story_flag(1, 1, "first", true), "story flag erase moves a surviving record across player shards"); check_records();
        owners->world->visibility_changed(1, 1, true); check_records();
        const auto manager_revision = owners->lifecycle->state_revision();
        owners->lifecycle->set_admission({ga::Mode::space, true, false});
        expect(owners->lifecycle->state_revision() != manager_revision, "lifecycle entry owns admission bump");
        expect(must(state->state_hash()) == must(ga::native_state_hash(must(state->capture()))), "real lifecycle mutation matches full oracle");
        check_records();
        const auto execution_revision = owners->execution->state_revision();
        must(owners->execution->replace({}));
        expect(owners->execution->state_revision() != execution_revision, "execution replacement owns bump even for same bytes");
        check_records();
        const auto production_revision = owners->production->state_revision();
        must(owners->production->restore(prepared));
        expect(owners->production->state_revision() != production_revision, "production restore owns epoch bump");
        check_records();
        must(state->restore(prepared)); check_records();
        expect(must(state->state_hash()) == must(ga::native_state_hash(prepared)), "restore returns exact hash despite differing process epochs");
    }
}
struct AfterWorldEffect final : auth::GalacticScriptEngine {
    galactic::GalacticSession* world{};
    core::Result<auth::ServiceOptions> before_service(const eawr::sim::PartitionExecutor&,
        const galactic::GalacticSession&, const galactic::GalacticTick&, auth::ScriptScheduler&) override {
        auto result = world->add_credits(1, whole(3), whole(1));
        if (!result) return core::Result<auth::ServiceOptions>::failure(result.error());
        return core::Result<auth::ServiceOptions>::success({});
    }
};
void world_hash_boundaries() {
    for (const auto count : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter executor(count);
        auto world = must(galactic::GalacticSession::create(fixture()));
        if (count > 1) world.set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
        const auto before = world.cached_state_hash();
        expect(before.get() == world.state_sha256(), "initial cached world hash matches independent hash");
        const auto stepped = must(world.step(executor)).state_hash;
        expect(stepped.get() == world.state_sha256(), "ordinary cached world hash matches independent hash");
        must(world.add_credits(1, whole(3), whole(1)));
        const auto after = world.cached_state_hash();
        expect(after.get() != stepped.get() && after.get() == world.state_sha256(), "post-step native cash invalidates correct world section");
        expect(before.get() != after.get(), "pending old hash retains its captured generation");
        const auto next = must(world.step(executor)).state_hash.get();
        expect(next == world.state_sha256(), "hash read does not lose dirtiness for next world service");
        auth::SessionConfig config; config.galactic_clock = true; config.tick_duration = {1, 30};
        auto hosted = must(auth::ScriptedGalacticSession::create(must(galactic::GalacticSession::create(fixture())),
            must(auth::ScriptScheduler::create(config, {})), must(auth::ScriptScheduler::create(config, {}))));
        if (count > 1) hosted.set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
        auto effect = std::make_shared<AfterWorldEffect>(); effect->world = &hosted.parent_world(); must(hosted.set_ai_engine(effect));
        const auto tick = must(hosted.step(executor));
        const auto expected = auth::authoritative_state_sha256(hosted.completed_tick(), hosted.world().state_sha256(), must(hosted.script_state_hash()));
        expect(tick.state_hash.get() == expected, "script bridge includes native cash admitted after its world step");
    }
}
std::string game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) throw std::runtime_error("installed composition requires game data");
    return std::move(*root);
}
void benchmark(const std::shared_ptr<cp::ComposedGalacticAiState>& state, unsigned workers, std::string_view label) {
    const auto saved = must(state->capture());
    const auto encoded = must(ga::encode_native_state(saved));
    const auto expected = must(ga::native_state_hash(saved));
    expect(must(state->state_hash()) == expected, "benchmark starts at independently verified boundary");
    const auto counts = state->owner_encodes();
    std::vector<double> full, incremental;
    for (unsigned i = 0; i < 120; ++i) {
        auto start = std::chrono::steady_clock::now();
        expect(must(ga::encode_native_state(must(state->capture()))) == encoded, "old full capture/encode/compare unchanged");
        full.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        start = std::chrono::steady_clock::now();
        expect(must(state->state_hash()) == expected, "benchmark incremental equals full oracle");
        incremental.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(full.begin(), full.end()); std::sort(incremental.begin(), incremental.end());
    expect(state->owner_encodes() == counts, "unchanged benchmark does zero contributor encoding");
    std::cout << "digest benchmark: state=" << label << " workers=" << workers << " bytes=" << encoded.size()
        << " before_median_ms=" << full[60] << " after_median_ms=" << incremental[60] << '\n';
}
void installed(bool measure = false) {
    namespace vfs = eawr::vfs;
    std::vector<vfs::MountSpec> mounts;
    for (const auto folder : {"corruption", "GameData"}) mounts.push_back(must(vfs::resolve_manifest_mount(folder, std::filesystem::path(game_root()) / folder / "Data")).mount);
    auto files = must(vfs::Vfs::mount(mounts));
    const auto read = [&](std::string_view path) { const auto bytes = must(files.open(path)); return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()); };
    ga::LifecycleSetup lifecycle;
    for (const auto& path : ga::required_xml()) lifecycle.xml[path] = read(path);
    std::map<std::string, std::string> modules;
    for (const auto& asset : must(files.enumerate("data/scripts", ".lua"))) {
        if (asset.canonical_path.starts_with("data/scripts/library/") || (asset.canonical_path.starts_with("data/scripts/ai/") && asset.canonical_path.substr(16).find('/') == std::string::npos))
            modules.emplace(asset.canonical_path, read(asset.canonical_path));
    }
    const auto plans = ai::galactic_goals::goal_plan_registrations(must(ai::galactic_goals::load_plan_catalog({}, modules)));
    const auto loaded = must(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    const auto data = must(eawr::data::galactic::load_galactic_data(files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    constexpr std::string_view campaign_name = "Sandbox_Equal_Footing_Empire";
    const auto* source = data.campaign(campaign_name); if (!source) throw std::runtime_error("stock campaign absent");
    cp::CampaignSetupOptions options; options.seed = 42;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<galactic::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == source->starting_active_player;
        if (human) options.solo_reference_human = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({{id}, id, human ? galactic::SeatKind::human : galactic::SeatKind::ai, id - 1, true, human ? id : 0});
    }
    std::vector<std::string> originals; for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});
    const auto planned = must(cp::plan_live_campaign_setup(files, data, types, campaign_name, options, "Normal_Default"));
    const auto services = must(cp::prepare_live_campaign_services(files, loaded.catalog, data, types, planned, "Normal_Default"));
    ai::GalacticPerceptionSetup perception; perception.seed = 42;
    galactic::PlayerId empire{};
    for (const auto& binding : planned.player_bindings()) {
        const auto* faction = data.faction(binding.faction);
        ai::GalacticPlayerFacts row{binding.player.player_id, binding.player.player_id, binding.faction, "Normal", faction->is_neutral.value_or(false), binding.faction == "Empire", false, 0};
        if (row.ai) empire = row.player;
        for (const auto& home : planned.homes()) if (home.player == row.player && home.planet) row.home = *home.planet;
        perception.players.push_back(row);
    }
    if (!empire) throw std::runtime_error("stock Empire player absent");
    lifecycle.players = {{empire, "BASICEMPIRE", "NORMAL", {}, {}}};
    for (const auto& location : planned.locations()) if (location.planet) perception.planets.push_back({location.planet->id, location.original_type, ai::crc32(ai::upper_case(location.original_type)), {}, {}, {}, {}, {}});
    for (const auto& binding : planned.type_bindings()) if (const auto* type = planned.original_type(binding.id)) {
        foc::AiType facts; facts.type_id = binding.id; facts.name = ai::upper_case(type->name); perception.types.push_back(std::move(facts));
    }
    std::string reference;
    std::vector<std::string> frame_reference;
    for (const auto count : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(count);
        auto campaign = must(cp::CampaignRoot::create(planned, services)); std::shared_ptr<RealOwners> owners;
        auto state = must(cp::install_composed_galactic_ai(*campaign.game(), [&](auto& world) {
            owners = std::make_shared<RealOwners>(world, lifecycle, perception, plans, executor);
            return core::Result<cp::GalacticAiContributors>::success(owners->contributors());
        }));
        const auto ai_data = must(ga::load_data(lifecycle)); std::vector<ga::PlanInput> registered;
        for (const auto& plan : plans) registered.push_back({plan.name, plan.goals});
        const auto& player_type = ai_data.players.at("BASICEMPIRE");
        const auto policy = ga::apply_template(ai_data, registered, {}, ai_data.templates.at(player_type.galactic_templates.front()));
        must(owners->engine->apply_galactic_policy(empire, policy));
        std::vector<ai::GoalFunction> functions;
        for (const auto& name : player_type.function_sets) if (const auto at = ai_data.function_sets.find(name); at != ai_data.function_sets.end())
            for (const auto& f : at->second) if (ai_data.goals.contains(f.goal) && ai_data.goals.at(f.goal).mode == "GALACTIC") functions.push_back(f);
        for (std::size_t index = 0; index < functions.size(); ++index) {
            const auto& goal = ai_data.goals.at(functions[index].goal);
            if (goal.application.contains("GLOBAL")) must(owners->engine->allocate_galactic_goal(empire, index, 0));
            else for (const auto target : *owners->engine->target_list(empire)) {
                if (owners->engine->target_matches(empire, goal.application, owners->engine->target(target))) must(owners->engine->allocate_galactic_goal(empire, index, target));
            }
        }
        expect(!owners->engine->galactic_goals(empire).empty(), "stock boundary contains real positive authored goal allocations");
        const auto hash = must(state->state_hash());
        if (reference.empty()) reference = hash; else expect(reference == hash, "stock composed prepared state equal on 1 and 8 threads");
        const auto saved = must(state->capture());
        const auto saved_bytes = must(ga::encode_native_state(saved)).size();
        if (measure) benchmark(state, count, "stock-prepared");
        std::vector<double> times;
        std::vector<std::string> frame_hashes;
        constexpr unsigned frames = 120;
        // A frame requests the sync digest; full verification also requests
        // the canonical digest before the explicit measurement below.
        const unsigned game_requests_per_frame = state->full_verification_enabled() ? 2U : 1U;
        const auto request_start = state->digest_requests();
        for (unsigned i = 0; i < frames; ++i) {
            // Real stock world progression with verification on every tick;
            // the gameplay services intentionally remain uncomposed.
            frame_hashes.push_back(must(campaign.game()->step(executor)).state_hash.get());
            const auto start = std::chrono::steady_clock::now();
            expect(must(state->state_hash()) == hash, "stock unchanged native digest reused");
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(times.begin(), times.end());
        const auto requests = state->digest_requests() - request_start;
        expect(requests == (game_requests_per_frame + 1) * frames,
            "sync and optional canonical verification requests per game frame plus one measurement request");
        if (frame_reference.empty()) frame_reference = frame_hashes; else expect(frame_reference == frame_hashes, "stock world progression hashes equal on 1/2/4/8 with full hash verification");
        expect(state->digest_generations() == 1, "stock unchanged requests hash once");
        must(state->restore(saved));
        expect(must(state->state_hash()) == hash, "all six real stock owners restore exact prepared state");
        std::cout << "composed stock prepared boundary: workers=" << count << " goals=" << owners->engine->galactic_goals(empire).size()
            << " bytes=" << saved_bytes << " game_requests_per_frame=" << game_requests_per_frame
            << " measurement_requests_per_frame=1 median_capture_encode_compare_ms=" << times[times.size() / 2]
            << " digest_generations=" << state->digest_generations() << '\n';
        if (measure) {
            expect(ai::EngineContractAccess::enlarge(*owners->engine, empire) >= 32, "larger synthetic state has several goals and tasks");
            benchmark(state, count, "synthetic-32-goals-tasks");
        }
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--installed") installed();
        else if (argc == 2 && std::string_view(argv[1]) == "--benchmark") installed(true);
        else { record_sum_contracts(); sync_contracts(); contracts(); real_boundaries(); world_hash_boundaries(); native_action_frontiers(); }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return failures ? 1 : 0;
}
