#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <utility>
#include "../support/check.hpp"
// S9-early contracts: per-player galactic random streams and per-player identity ranges for plans,
// targets, task forces and plan-script instances. Tactical players share the stream mechanism while
// keeping engine-wide identity counters (GFS-A24). Synthetic data only; the 600-frame and 1/2/4/8 acceptance runs use the stock
// campaign tests.
#include "ai_s9_fixture.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

namespace eawr::script::foc::ai {
// Reads and stages engine state the public API does not expose.
struct EngineContractAccess {
    static void retain_positioning(Engine& e) { e.player_ai(2)->positioning[99] = {first_plan_instance, 70, 20}; }
    static bool positioning_retained(const Engine& e) {
        const auto& rows = e.player_ai(2)->positioning;
        return rows.contains(99) && rows.at(99).instance == first_plan_instance && rows.at(99).next == 70 && rows.at(99).target == 20;
    }

    static std::uint32_t seed(const Engine& e, tactical::PlayerId player) { return e.player_ai(player)->sync.seed(); }
    static std::uint32_t engine_seed(const Engine& e) { return e.sync_.seed(); }
    static std::uint32_t slot(const Engine& e, tactical::PlayerId player) { return e.player_ai(player)->slot; }
    static void mutable_lookup(Engine& e, tactical::PlayerId player) { static_cast<void>(e.player_ai(player)); }
    static std::vector<std::uint32_t> draw(Engine& e, tactical::PlayerId player, unsigned count) {
        const auto native_record_write = e.native_record_scope();
        e.native_records_changed(player, {1});
        e.mark_native_state_changed();
        std::vector<std::uint32_t> values;
        for (unsigned i = 0; i < count; ++i) values.push_back(e.stream(*e.player_ai(player)).next());
        return values;
    }
    static std::optional<std::uint64_t> target(Engine& e, tactical::PlayerId player) { return e.allocate_target_id(*e.player_ai(player)); }
    static std::optional<std::uint64_t> taskforce(Engine& e, tactical::PlayerId player) { return e.allocate_taskforce_id(*e.player_ai(player)); }
    static std::optional<std::uint64_t> plan(Engine& e, tactical::PlayerId player) { return e.allocate_plan_id(*e.player_ai(player)); }
    static std::optional<std::uint64_t> instance(Engine& e, tactical::PlayerId player) { return e.allocate_instance_id(*e.player_ai(player)); }
    static bool target_issued(const Engine& e, std::uint64_t id) { return e.galactic_target_issued(id); }
    static bool taskforce_issued(const Engine& e, std::uint64_t id) { return e.galactic_taskforce_issued(id); }
    static bool plan_issued(const Engine& e, std::uint64_t id) { return e.galactic_plan_issued(id); }
    static bool instance_issued(const Engine& e, std::uint64_t id) { return e.galactic_instance_issued(id); }
    static void exhaust_plans(Engine& e, tactical::PlayerId player) {
        const auto native_record_write = e.native_record_scope();
        e.native_records_changed(player, {1});
        e.mark_native_state_changed();
        e.player_ai(player)->next_plan_local = galactic_id_counter_limit;
    }
    static void exhaust_instances(Engine& e, tactical::PlayerId player) {
        const auto native_record_write = e.native_record_scope();
        e.native_records_changed(player, {1});
        e.mark_native_state_changed();
        e.player_ai(player)->next_instance_local = galactic_instance_counter_limit;
    }
    static void exhaust_taskforces(Engine& e, tactical::PlayerId player) {
        const auto native_record_write = e.native_record_scope();
        e.native_records_changed(player, {1});
        e.mark_native_state_changed();
        e.player_ai(player)->next_taskforce_local = galactic_id_counter_limit;
    }
    static std::uint64_t next_plan_local(const Engine& e, tactical::PlayerId player) { return e.player_ai(player)->next_plan_local; }
    static void set_next_plan_local(Engine& e, tactical::PlayerId player, std::uint64_t value) {
        const auto native_record_write = e.native_record_scope();
        e.native_records_changed(player, {1});
        e.mark_native_state_changed();
        e.player_ai(player)->next_plan_local = value;
    }
    static void set_slot(Engine& e, tactical::PlayerId player, std::uint32_t value) {
        const auto native_record_write = e.native_record_scope();
        e.native_records_changed(player, {1});
        e.mark_native_state_changed();
        e.player_ai(player)->slot = value;
    }
    static void move_potential(Engine& e, tactical::PlayerId from, std::uint64_t goal, std::uint64_t id) {
        const auto native_record_write = e.native_record_scope();
        e.native_records_changed(from, {2, 3});
        e.mark_native_state_changed();
        for (auto& row : e.player_ai(from)->proposed) if (row.id == goal) row.potential->id = id;
    }
    struct RuntimeIds {
        tactical::PlayerId player{};
        std::uint64_t plan{}, force{}, block{}, instance{}, alternate{};
    };
    // Record-inventory fixture, independent of live goal admission/save validation.
    static std::vector<RuntimeIds> runtime_inventory(Engine& e, std::size_t count) {
        const auto write = e.native_record_scope(); e.native_records_replaced();
        std::vector<RuntimeIds> selected;
        for (auto& owner : e.players_) {
            RuntimeIds first; first.player = owner.player;
            for (std::size_t i = 0; i < count; ++i) {
                Plan plan; plan.id = *e.allocate_plan_id(owner); plan.instance = *e.allocate_instance_id(owner);
                plan.player = owner.player;
                TaskForce force; force.id = *e.allocate_taskforce_id(owner); force.plan = plan.id;
                plan.taskforces = {force.id};
                Block block; block.id = block_id(plan.instance, 17); block.instance = plan.instance; block.taskforce = force.id;
                owner.taskforces.emplace(force.id, force); owner.blocks.emplace(block.id, block);
                owner.plan_of_instance.emplace(plan.instance, plan.id); owner.running.emplace(plan.id, plan);
                if (i == 0) { first.plan = plan.id; first.force = force.id; first.block = block.id; first.instance = plan.instance; }
                if (i == 1) first.alternate = plan.id;
            }
            selected.push_back(first);
        }
        // Preserve the former record owner derived from the foreign TaskForce's plan,
        // rather than confusing the storage shard with that record's semantic owner.
        e.player_ai(selected[0].player)->blocks.at(selected[0].block).taskforce = selected[1].force;
        e.rebuild_runtime_index(); e.mark_native_state_changed(); return selected;
    }
    static void runtime_record_write(Engine& e, const RuntimeIds& ids, std::uint32_t kind) {
        const auto write = e.native_record_scope();
        auto& owner = *e.player_ai(ids.player);
        const auto key = kind == 21 ? ids.force : kind == 22 ? ids.plan : kind == 23 ? ids.block : ids.instance;
        e.native_record_changed(Engine::native_all_players, kind, key);
        if (kind == 21) owner.taskforces.at(ids.force).had_units = !owner.taskforces.at(ids.force).had_units;
        if (kind == 22) owner.running.at(ids.plan).result = !owner.running.at(ids.plan).result;
        if (kind == 23) owner.blocks.at(ids.block).ready = !owner.blocks.at(ids.block).ready;
        if (kind == 24) owner.plan_of_instance.at(ids.instance) = ids.alternate;
        e.mark_native_state_changed();
    }
    static bool record_owner(const Engine& e, tactical::PlayerId owner, std::uint32_t kind, std::uint64_t id) {
        core::RecordDigest::Bytes key; core::RecordDigest::append(key, id);
        return e.native_record_rows_.contains(Engine::NativeRecordKey{owner, kind, key});
    }
    static void retained_globals(Engine& e, std::size_t count) {
        const auto write = e.native_record_scope();
        e.native_records_changed(0, {27});
        e.globals_.clear();
        for (std::size_t i = 0; i < count; ++i) e.globals_.emplace("GLOBAL_" + std::to_string(i), real(static_cast<std::int64_t>(i)));
        e.mark_native_state_changed();
    }
    static void retained_targets(Engine& e, std::size_t count) {
        const auto write = e.native_record_scope();
        e.native_records_changed(0, {20});
        for (std::size_t i = 0; i < count; ++i) {
            Target target; target.id = *e.allocate_target_id(e.players_.front());
            e.targets_.emplace(target.id, std::move(target));
        }
        e.mark_native_state_changed();
    }
    static void header_write(Engine& e) {
        const auto write = e.native_record_scope();
        ++e.sequence_; e.mark_native_state_changed();
    }
    static void target_write(Engine& e, std::uint64_t id, bool remove) {
        const auto write = e.native_record_scope();
        e.native_record_changed(0, 20, id);
        if (remove) e.targets_.erase(id); else e.targets_.at(id).name += "_CHANGED";
        e.mark_native_state_changed();
    }
    // A tactical player: stream() is per player, while identity allocators stay engine-wide.
    struct Tactical {
        std::uint32_t engine_seed_before{}, engine_seed_after{}, player_seed_before{}, player_seed_after{};
        std::uint64_t target_a{}, target_b{}, taskforce_a{}, plan_a{}, plan_b{}, instance_a{};
    };
    static Tactical tactical_fallback(Engine& e) {
        const auto native_record_write = e.native_record_scope();
        e.mark_native_state_changed();
        PlayerAi player; player.player = 1; player.galactic = false;
        Tactical result;
        result.engine_seed_before = e.sync_.seed(); result.player_seed_before = player.sync.seed();
        static_cast<void>(e.stream(player).next());
        result.engine_seed_after = e.sync_.seed(); result.player_seed_after = player.sync.seed();
        result.target_a = *e.allocate_target_id(player); result.target_b = *e.allocate_target_id(player);
        result.taskforce_a = *e.allocate_taskforce_id(player);
        result.plan_a = *e.allocate_plan_id(player); result.plan_b = *e.allocate_plan_id(player);
        result.instance_a = *e.allocate_instance_id(player);
        return result;
    }
};
}

namespace {
namespace ai = s9_fixture::ai;
namespace gc = s9_fixture::gc;
using s9_fixture::must;
using Access = ai::EngineContractAccess;
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool ok, std::string_view message) {
    if (!ok) local_checker.check_before_print(ok, message, std::cerr, "FAIL: ");
}

constexpr std::uint64_t two_pow_32 = std::uint64_t{1} << 32;
constexpr std::uint64_t two_pow_53 = std::uint64_t{1} << 53;
// Layout constants the constraints rest on, checked at compile time.
static_assert(ai::galactic_instance_base(ai::galactic_id_slots - 1) + ai::galactic_instance_counter_limit < two_pow_32,
    "plan-script instances stay below 2^32 for every slot");
static_assert(ai::galactic_instance_base(0) == ai::first_plan_instance, "slot 0 starts at the first plan instance");
static_assert(ai::galactic_id_base(ai::galactic_id_slots - 1) + ai::galactic_id_counter_limit <= two_pow_53,
    "identities survive a binary64 number");
static_assert(ai::galactic_id_slot(ai::galactic_id_base(5) | 77) == 5 && ai::galactic_id_slot(77) == ai::galactic_id_slots
    && ai::galactic_id_slot(ai::galactic_id_base(ai::galactic_id_slots)) == ai::galactic_id_slots, "slot decoding");
static_assert(ai::galactic_instance_slot(ai::galactic_instance_base(9) + 3) == 9 && ai::galactic_instance_slot(1) == ai::galactic_id_slots,
    "instance slot decoding");

void seeding() {
    eawr::platform::ThreadWorkerAdapter executor(2);
    s9_fixture::Shape shape; shape.first = 7; shape.second = 9; shape.type = "Zeta"; shape.first_faction = "Alpha"; shape.second_faction = "Omega";
    s9_fixture::Owner owner(shape); owner.begin(executor);
    expect(Access::seed(*owner.engine, 7) == ai::galactic_stream_seed(42, 7) && Access::seed(*owner.engine, 9) == ai::galactic_stream_seed(42, 9),
        "each galactic player's stream is seeded from (campaign seed, player ID)");
    expect(Access::seed(*owner.engine, 7) != Access::seed(*owner.engine, 9), "two players never share a stream");
    expect(Access::engine_seed(*owner.engine) == 42, "the engine-wide stream keeps the plain campaign seed");
    expect(Access::slot(*owner.engine, 7) == 0 && Access::slot(*owner.engine, 9) == 1, "slots follow the setup's player list");
    s9_fixture::Shape other = shape; other.seed = 43;
    s9_fixture::Owner reseeded(other); reseeded.begin(executor);
    expect(Access::seed(*reseeded.engine, 7) != Access::seed(*owner.engine, 7), "a different campaign seed changes the stream");
    // Distinct neighbouring seeds and players must not produce the same early draws.
    auto left = Access::draw(*owner.engine, 7, 6), right = Access::draw(*owner.engine, 9, 6);
    expect(left != right, "neighbouring player streams do not repeat each other");
}

void independence() {
    eawr::platform::ThreadWorkerAdapter executor(1);
    {
        // Drawing from player A must not change player B's next value, in either order.
        s9_fixture::Owner ab, ba; ab.begin(executor); ba.begin(executor);
        const auto a_first = Access::draw(*ab.engine, 1, 5); const auto b_second = Access::draw(*ab.engine, 2, 5);
        const auto b_first = Access::draw(*ba.engine, 2, 5); const auto a_second = Access::draw(*ba.engine, 1, 5);
        expect(a_first == a_second && b_first == b_second, "a player's sequence does not depend on the other player's draws");
        const auto before = Access::seed(*ab.engine, 2);
        static_cast<void>(Access::draw(*ab.engine, 1, 3));
        expect(Access::seed(*ab.engine, 2) == before, "a draw by player A leaves player B's stream untouched");
    }
    {
        // The same through the real draw sites: goal allocation chooses a plan with the player's stream.
        s9_fixture::Owner control; control.begin(executor); control.policy(1); control.policy(2);
        const auto engine_before = Access::engine_seed(*control.engine);
        const auto second_before = Access::seed(*control.engine, 2), first_before = Access::seed(*control.engine, 1);
        const auto first = control.allocate(1, 10);
        expect(first && Access::seed(*control.engine, 1) != first_before, "allocating a goal advances the allocating player's stream");
        expect(Access::seed(*control.engine, 2) == second_before, "allocating a goal leaves the other player's stream untouched");
        expect(Access::engine_seed(*control.engine) == engine_before, "galactic goal allocation no longer draws from the engine stream");
        // Order independence of the outcome: player 2 allocating first changes nothing for player 1.
        s9_fixture::Owner flipped; flipped.begin(executor); flipped.policy(1); flipped.policy(2);
        static_cast<void>(flipped.allocate(2, 20));
        const auto again = flipped.allocate(1, 10);
        expect(again && first && again->definition == first->definition && again->potential_plan == first->potential_plan,
            "player 1's chosen plan and identity do not depend on whether player 2 allocated first");
        expect(Access::seed(*flipped.engine, 1) == Access::seed(*control.engine, 1), "player 1's stream ends in the same place");
    }
}

void identities() {
    eawr::platform::ThreadWorkerAdapter executor(1);
    s9_fixture::Owner owner; owner.begin(executor); owner.policy(1); owner.policy(2);
    auto& e = *owner.engine;
    std::vector<std::uint64_t> seen;
    const auto fresh = [&](std::uint64_t id) { const bool ok = std::find(seen.begin(), seen.end(), id) == seen.end(); seen.push_back(id); return ok; };
    for (const std::uint32_t player : {1U, 2U}) {
        const auto slot = Access::slot(e, player);
        const auto target = Access::target(e, player), taskforce = Access::taskforce(e, player), plan = Access::plan(e, player), instance = Access::instance(e, player);
        expect(target && taskforce && plan && instance, "fresh ranges allocate");
        expect(*taskforce == (ai::galactic_id_base(slot) | 1) && *plan == (ai::galactic_id_base(slot) | 1),
            "task-force and plan identities start at the first number of the player's range");
        expect(*instance == ai::galactic_instance_base(slot), "instances start at the player's instance base");
        expect(ai::galactic_id_slot(*target) == slot && ai::galactic_instance_slot(*instance) == slot, "the slot is recoverable from the identity");
        expect(*instance >= ai::first_plan_instance && *instance < two_pow_32, "an instance fits the block identity's upper 32 bits");
        expect(*target < two_pow_53 && *plan < two_pow_53, "an identity survives a binary64 number");
        expect(Access::target_issued(e, *target) && Access::taskforce_issued(e, *taskforce) && Access::plan_issued(e, *plan)
            && Access::instance_issued(e, *instance), "an allocated identity is inside its owner's range");
        expect(!Access::target_issued(e, *target + 1) && !Access::plan_issued(e, *plan + 1) && !Access::instance_issued(e, *instance + 1),
            "an identity past the counter is not issued");
        expect(fresh(*target) && fresh(*instance), "no two players ever share a target or instance identity");
        expect(!Access::target_issued(e, 1) && !Access::plan_issued(e, 0) && !Access::instance_issued(e, ai::first_plan_instance - 1),
            "legacy engine-wide numbers are not in any galactic range");
    }
    // Targets are shared by every list; the player whose list first names one allocates it, here the lowest ID.
    expect(ai::galactic_id_slot(e.target_of_object(10)->id) == 0 && ai::galactic_id_slot(e.target_of_object(20)->id) == 0,
        "a target takes its identity from the range of the player that first listed it");
    // The potential and the running plan of one player come out of one counter.
    const auto goal = owner.allocate(1, 10);
    expect(goal && Access::plan_issued(e, goal->potential_plan) && ai::galactic_id_slot(goal->potential_plan) == 0,
        "a potential plan identity comes from the player's range");
    const auto next_running = Access::plan(e, 1);
    expect(goal && next_running && *next_running == goal->potential_plan + 1, "potential and running plans share one allocator per player");
    expect(owner.allocate(2, 20)->potential_plan != goal->potential_plan, "two players' plan identities differ");
}

void exhaustion() {
    // Deterministic refusal: a full range never wraps into another player's range.
    eawr::platform::ThreadWorkerAdapter executor(1);
    s9_fixture::Owner owner; owner.begin(executor); owner.policy(1); owner.policy(2);
    auto& e = *owner.engine;
    Access::exhaust_plans(e, 1);
    expect(!Access::plan(e, 1) && Access::next_plan_local(e, 1) == ai::galactic_id_counter_limit, "an exhausted plan range refuses without wrapping");
    const auto refused = e.allocate_galactic_goal(1, 0, e.target_of_object(10)->id);
    expect(!refused && refused.error().message.find("identity space exhausted") != std::string::npos, "goal allocation refuses on an exhausted plan range");
    expect(Access::plan(e, 2).has_value() && owner.allocate(2, 20).has_value(), "another player's range is unaffected");
    Access::exhaust_instances(e, 2); Access::exhaust_taskforces(e, 2);
    expect(!Access::instance(e, 2) && !Access::taskforce(e, 2), "exhausted instance and task-force ranges refuse");
}

void archive() {
    eawr::platform::ThreadWorkerAdapter executor(1);
    const auto verified_records = [&](const ai::GoalStateContributor& contributor) {
        expect(must(contributor.record_sync_hash()) == must(contributor.record_sync_hash(true)),
            "per-player streams and identity writes maintain the native record sums");
    };
    s9_fixture::Owner control; control.begin(executor);
    ai::GoalStateContributor contributor(control.engine, s9_fixture::identity());
    control.policy(1); control.policy(2);
    verified_records(contributor);
    const auto first = control.allocate(1, 10); const auto other = control.allocate(2, 20);
    expect(first && other, "goals allocated");
    static_cast<void>(Access::draw(*control.engine, 2, 4));
    verified_records(contributor);
    const auto bytes = must(contributor.capture());
    // A fresh owner restores the per-player streams and ranges exactly and then continues in step.
    s9_fixture::Owner resumed; must(resumed.world->bind(s9_fixture::snapshot(resumed.shape), {}, executor, 0, 1000));
    ai::GoalStateContributor loader(resumed.engine, s9_fixture::identity());
    resumed.begin(executor);
    Access::retain_positioning(*resumed.engine);
    must(loader.restore(bytes));
    expect(Access::positioning_retained(*resumed.engine), "galactic restore preserves positioning rows omitted from its native component");
    verified_records(loader);
    expect(must(loader.record_sync_hash()) == must(contributor.record_sync_hash()), "restored record sums match the uninterrupted owner");
    expect(must(loader.capture()).components == bytes.components, "streams and ranges re-encode byte-identically");
    for (const std::uint32_t player : {1U, 2U})
        expect(Access::seed(*resumed.engine, player) == Access::seed(*control.engine, player)
            && Access::slot(*resumed.engine, player) == Access::slot(*control.engine, player), "per-player seeds and slots are restored");
    const auto clean_work = control.engine->native_record_work();
    {
        const auto clean_scope = control.engine->native_record_scope();
        Access::mutable_lookup(*control.engine, 1);
    }
    expect(clean_work == control.engine->native_record_work(), "a clean scope and mutable lookup encode no records");
    const auto before_work = control.engine->native_record_work();
    const auto before_hash = must(contributor.record_sync_hash());
    expect(Access::plan(*control.engine, 1) == Access::plan(*resumed.engine, 1), "restored plan allocator continues");
    verified_records(contributor); verified_records(loader);
    expect(Access::target(*control.engine, 2) == Access::target(*resumed.engine, 2), "restored target allocator continues");
    verified_records(contributor); verified_records(loader);
    expect(Access::taskforce(*control.engine, 2) == Access::taskforce(*resumed.engine, 2), "restored task-force allocator continues");
    verified_records(contributor); verified_records(loader);
    expect(Access::instance(*control.engine, 1) == Access::instance(*resumed.engine, 1), "restored instance allocator continues");
    verified_records(contributor); verified_records(loader);
    const auto after_work = control.engine->native_record_work();
    expect(before_hash != must(contributor.record_sync_hash()), "allocator counter writes change the native digest");
    for (std::size_t kind = 2; kind < before_work.size(); ++kind)
        expect(before_work[kind] == after_work[kind], "allocator writes encode no untouched goal, history, task or map records");
    expect(after_work[1] - before_work[1] == 4, "four allocator writes encode only their four owner headers");
    expect(Access::draw(*control.engine, 1, 6) == Access::draw(*resumed.engine, 1, 6)
        && Access::draw(*control.engine, 2, 6) == Access::draw(*resumed.engine, 2, 6), "restored streams continue the same values");
    verified_records(contributor); verified_records(loader);
    expect(must(loader.record_sync_hash()) == must(contributor.record_sync_hash()), "continued streams and allocators keep equal native sums");
    // The per-player range checks reject what the old engine-wide check would have accepted.
    s9_fixture::Owner tampered; tampered.begin(executor); tampered.policy(1); tampered.policy(2);
    const auto goal = tampered.allocate(1, 10); static_cast<void>(tampered.allocate(2, 20));
    ai::GoalStateContributor probe(tampered.engine, s9_fixture::identity());
    expect(static_cast<bool>(probe.capture()), "an untouched state validates");
    Access::set_next_plan_local(*tampered.engine, 1, 1);
    expect(!probe.capture(), "a retained plan identity at or past its owner's counter refuses");
    Access::set_next_plan_local(*tampered.engine, 1, 2);
    expect(static_cast<bool>(probe.capture()), "counter just past the retained identity validates");
    Access::move_potential(*tampered.engine, 1, goal->goal, ai::galactic_id_base(1) | 1);
    expect(!probe.capture(), "a plan identity inside another player's range refuses");
    Access::move_potential(*tampered.engine, 1, goal->goal, ai::galactic_id_base(0) | 1);
    Access::set_slot(*tampered.engine, 1, 1);
    expect(!probe.capture(), "a slot that is not the player's position in the setup refuses");
}

void bounded_record_reconciliation() {
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        s9_fixture::Owner owner; owner.begin(executor);
        ai::GoalStateContributor contributor(owner.engine, s9_fixture::identity());
        const auto verify = [&] { expect(must(contributor.record_sync_hash()) == must(contributor.record_sync_hash(true)), "selected reconciliation equals independent cold codec"); };
        for (const std::size_t size : {1U, 1024U}) {
            Access::retained_globals(*owner.engine, size); verify();
            const auto before = owner.engine->native_record_inspections();
            const auto old_hash = must(contributor.record_sync_hash());
            Access::header_write(*owner.engine);
            expect(owner.engine->native_record_inspections() - before == 2, "header-only write inspects one current and one retained record regardless of unrelated rows");
            expect(old_hash != must(contributor.record_sync_hash()), "changed header reaches its warm digest"); verify();
        }
        Access::retained_targets(*owner.engine, 1024); verify();
        const auto id = owner.engine->target_of_object(10)->id;
        for (const bool remove : {false, true}) {
            const auto before = owner.engine->native_record_inspections();
            Access::target_write(*owner.engine, id, remove);
            expect(owner.engine->native_record_inspections() - before == (remove ? 3U : 4U), "exact target change/removal touches only its row and runtime header"); verify();
        }
    }
}

void owned_runtime_records() {
    for (const auto count : {2U, 64U}) for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        s9_fixture::Owner owner; owner.begin(executor);
        const auto rows = Access::runtime_inventory(*owner.engine, count);
        ai::GoalStateContributor contributor(owner.engine, s9_fixture::identity());
        const auto verify = [&] { expect(must(contributor.record_sync_hash()) == must(contributor.record_sync_hash(true)),
            "owned runtime record sums match the independent canonical field walk"); };
        verify();
        expect(Access::record_owner(*owner.engine, rows[1].player, 23, rows[0].block),
            "foreign TaskForce block keeps its semantic plan owner rather than its storage shard");
        for (const auto& row : rows) for (const auto kind : {21U, 22U, 23U, 24U}) {
            const auto before = owner.engine->native_record_inspections();
            const auto work = owner.engine->native_record_work();
            Access::runtime_record_write(*owner.engine, row, kind);
            expect(owner.engine->native_record_inspections() - before == 4,
                "one owned runtime mutation inspects only its current/retained row and header, independent of unrelated rows");
            expect(owner.engine->native_record_work()[kind] == work[kind] + 1,
                "one owned runtime mutation encodes exactly its selected row");
            verify();
        }
    }
}

void tactical_unchanged() {
    eawr::platform::ThreadWorkerAdapter executor(1);
    s9_fixture::Owner owner; owner.begin(executor);
    const auto result = Access::tactical_fallback(*owner.engine);
    expect(result.engine_seed_after == result.engine_seed_before && result.player_seed_after != result.player_seed_before,
        "a tactical player draws from its own stream without advancing the engine stream");
    expect(result.target_a == 1 && result.target_b == 2 && result.taskforce_a == 1 && result.plan_a == 1 && result.plan_b == 2,
        "a tactical player allocates from the engine-wide counters");
    expect(result.instance_a == ai::first_plan_instance, "a tactical instance starts at the first plan instance");
}
} // namespace

int case_galactic_ai_stream_ids_tests() {
    try {
        seeding(); independence(); identities(); exhaustion(); archive(); bounded_record_reconciliation(); owned_runtime_records(); tactical_unchanged();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures) return 1;
    std::cout << "galactic AI stream and identity contracts passed\n";
    return 0;
}
