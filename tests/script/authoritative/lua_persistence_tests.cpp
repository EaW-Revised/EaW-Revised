#include "lua_persistence_support.hpp"
// Save, load and state-hash tests of the authoritative script scheduler
// (#248, docs/lua-persistence.md): save -> load -> continue equals the
// uninterrupted run (commands, diagnostics, removals, state hashes) at every
// save point and worker count, re-saving a loaded state gives the same bytes,
// and corrupted, mismatched or over-quota saves fail without changing the
// scheduler.
//
//   lua_persistence_tests roundtrip | workers | corruption [trace] [seed rounds] | rejects
//   lua_persistence_tests dump [tick file]

#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/authoritative/clock_rng.hpp"
#include "eawr/sim/world.hpp"
#include "harness.hpp"
#include "persist_scenario.hpp"
#include "scenario.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Allocation probe: while armed, records the largest single allocation and the
// total allocated, so a test can show that a forged count drove no allocation.
namespace alloc_probe {
std::atomic<bool> armed{false};
std::atomic<std::size_t> largest{0};
std::atomic<std::size_t> total{0};
} // namespace alloc_probe

void* operator new(std::size_t size) {
    if (alloc_probe::armed.load(std::memory_order_relaxed)) {
        alloc_probe::total.fetch_add(size, std::memory_order_relaxed);
        std::size_t largest = alloc_probe::largest.load(std::memory_order_relaxed);
        while (size > largest && !alloc_probe::largest.compare_exchange_weak(largest, size, std::memory_order_relaxed)) {
        }
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}

// GCC reads free() of an operator-new pointer as a mismatch; this operator new
// is malloc.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace lua_persistence_test_support {


constexpr int total_ticks = 30;
// `corruption trace` names each attempt before it runs.
bool trace_attempts = false;
// State hash of the persistence scenario after the last tick, the same on every
// target (docs/lua-persistence.md): a change means the encoding or the script
// semantics changed.
constexpr std::string_view golden_final_hash = "46be48e271773f638988dd2308495d63ca848504578d0426810f7c6d9c325fc7";

auth::SessionConfig persistence_config() {
    auth::SessionConfig config = default_config();
    config.seed = 0x5A7E;
    return config;
}

Session persistence_session(auth::SessionConfig config) {
    Session session = make_session(
        {{"Persist.lua", persist_main}, {"Library/Kit.lua", persist_kit}, {"Exit.lua", persist_exit}}, std::move(config));
    expect(session.scheduler
               ->register_binding("Test_Self", [](auth::BindingContext& context, const auth::ValueList&) {
                   return eawr::core::Result<auth::ValueList>::success(
                       {auth::Value::number(integer(static_cast<std::int64_t>(context.instance())))});
               })
               .has_value(),
           "register Test_Self");
    return session;
}

// Host actions at the barrier after tick `tick` (0: before the first tick).
void barrier_actions(Session& session, int tick) {
    auto create = [&](std::uint64_t id, const char* module) {
        auto created = session.scheduler->create_instance(id, module);
        expect(created.has_value(), "create instance " + std::to_string(id) + ": " +
                                        (created ? std::string() : created.error().code + " " + created.error().message));
    };
    if (tick == 0) {
        create(5, "Persist.lua");
        create(9, "Persist.lua");
        create(12, "Exit.lua");
    }
    if (tick == 4) create(20, "Persist.lua");
    for (const std::uint64_t id : {std::uint64_t{5}, std::uint64_t{9}, std::uint64_t{20}}) {
        if ((static_cast<std::uint64_t>(tick) + id) % 3 != 0) continue;
        const auto key_tick = static_cast<std::uint64_t>(tick + 2);
        auth::ValueList arguments{auth::Value{auth::Handle{7, id + static_cast<std::uint64_t>(tick)}}, auth::Value::number(integer(tick))};
        expect(session.scheduler->submit_event(dispatch_event({key_tick, 30, id, static_cast<std::uint64_t>(tick) * 100 + id}, id,
                                                              "attacked", std::move(arguments)))
                   .has_value(),
               "submit event");
    }
}

std::string state_hash(Session& session) {
    auto hash = session.scheduler->state_hash();
    expect(hash.has_value(), "state hash: " + (hash ? std::string() : hash.error().code + " " + hash.error().message));
    return hash ? hash.value() : std::string();
}

// Output of one tick: its commands, diagnostics and removals.
std::string service_tick(Session& session, const eawr::sim::PartitionExecutor& executor) {
    const std::size_t commands = session.commands.size();
    const std::size_t diagnostics = session.diagnostics.size();
    const std::size_t removed = session.removed.size();
    step(session, executor);
    std::string out;
    for (std::size_t index = commands; index < session.commands.size(); ++index) out += "C " + session.commands[index] + "\n";
    for (std::size_t index = diagnostics; index < session.diagnostics.size(); ++index) out += "D " + session.diagnostics[index] + "\n";
    for (std::size_t index = removed; index < session.removed.size(); ++index) out += "R " + std::to_string(session.removed[index]) + "\n";
    return out;
}

struct Run {
    std::vector<std::string> ticks;  // output of tick t at index t - 1
    std::vector<std::string> hashes; // state hash at barrier t at index t
};

// Runs barriers first..last on `session` (already at barrier first - 1).
void continue_run(Session& session, const eawr::sim::PartitionExecutor& executor, int first, Run& run) {
    for (int tick = first; tick <= total_ticks; ++tick) {
        run.ticks.push_back(service_tick(session, executor));
        barrier_actions(session, tick);
        run.hashes.push_back(state_hash(session));
    }
}

Run reference_run(const eawr::sim::PartitionExecutor& executor) {
    Session session = persistence_session();
    Run run;
    barrier_actions(session, 0);
    run.hashes.push_back(state_hash(session));
    continue_run(session, executor, 1, run);
    return run;
}

// A session at barrier `tick`, and its save.
std::string save_at(int tick, const eawr::sim::PartitionExecutor& executor, Session* keep = nullptr) {
    Session session = persistence_session();
    barrier_actions(session, 0);
    for (int index = 1; index <= tick; ++index) {
        static_cast<void>(service_tick(session, executor));
        barrier_actions(session, index);
    }
    auto bytes = session.scheduler->save();
    expect(bytes.has_value(), "save at " + std::to_string(tick) + ": " + (bytes ? std::string() : bytes.error().message));
    std::string result = bytes ? bytes.value() : std::string();
    if (keep != nullptr) *keep = std::move(session);
    return result;
}

Session loaded(const std::string& bytes) {
    Session session = persistence_session();
    auto result = session.scheduler->load(bytes);
    expect(result.has_value(), "load: " + (result ? std::string() : result.error().code + " " + result.error().message));
    return session;
}

void compare_continuation(const Run& reference, const Run& continued, int saved, const std::string& label) {
    for (int tick = saved + 1; tick <= total_ticks; ++tick) {
        const std::size_t offset = static_cast<std::size_t>(tick - saved - 1);
        if (continued.ticks[offset] != reference.ticks[static_cast<std::size_t>(tick - 1)]) {
            expect(false, label + ": output of tick " + std::to_string(tick) + " differs\n--- reference\n" +
                              reference.ticks[static_cast<std::size_t>(tick - 1)] + "--- continued\n" + continued.ticks[offset]);
            return;
        }
        if (continued.hashes[offset] != reference.hashes[static_cast<std::size_t>(tick)]) {
            expect(false, label + ": state hash at tick " + std::to_string(tick) + " differs");
            return;
        }
    }
}

// ---- roundtrip ----

void pinned_manifest_identity() {
    auth::ModuleManifest modules;
    expect(modules.add("B.lua", "return 2").has_value(), "identity: add second module");
    expect(modules.add("./A.lua", std::string("return 1\0tail", 13)).has_value(), "identity: add binary module");
    const auto pinned_modules = modules;
    // The unchanged manifest algorithm is the oracle for the cached save identity.
    const auto recomputed = modules.digest();
    auto original = auth::ScriptScheduler::create(persistence_config(), modules);
    expect(original.has_value(), "identity: create scheduler");
    if (!original) return;
    const auto saved = original.value().save();
    expect(saved.has_value(), "identity: save scheduler");
    if (!saved) return;
    // The save contains the u32-length-framed digest, including embedded zero bytes.
    const std::string framed = std::string("\x40\0\0\0", 4) + recomputed;
    expect(saved.value().find(framed) != std::string::npos,
        "identity: cached digest equals the independently recomputed manifest digest");
    expect(modules.add("C.lua", "return 3").has_value(), "identity: extend caller's manifest");
    expect(modules.digest() != recomputed, "identity: caller's extended manifest has another digest");
    const auto pinned_save = original.value().save();
    expect(pinned_save && pinned_save.value() == saved.value(), "identity: caller edits do not change pinned save bytes");
    auto restored = auth::ScriptScheduler::create(persistence_config(), pinned_modules);
    expect(restored && restored.value().load(saved.value()).has_value(), "identity: same modules accept the save");
    if (restored) {
        const auto repeated = restored.value().save();
        expect(repeated && repeated.value() == saved.value(), "identity: load and re-save retain exact bytes");
    }
    auto changed = auth::ScriptScheduler::create(persistence_config(), std::move(modules));
    expect(changed.has_value(), "identity: create scheduler with changed modules");
    if (changed) {
        const auto refused = changed.value().load(saved.value());
        expect(!refused && refused.error().message.find("other modules") != std::string::npos,
            "identity: changed modules reject the cached save identity");
    }
}

void run_roundtrip() {
    std::string query_reference;
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const std::map<std::string, std::string> modules{{"Query.lua",
            "Root_Executed()\nRoots = (Roots or 0) + 1\ncount = 0\nfunction Query() count = count + 1; Test_Report(count, GetCurrentTime.Frame()) end\n"},
            {"Timer.lua", "Register_Event('Fired', function() Test_Report('fired', GetCurrentTime.Frame()) end)\nfunction Start() Test_Timer(1, 'Fired') end\n"}};
        auto config = default_config(); config.query_clock = true;
        auto root_executions = std::make_shared<unsigned>(0);
        const auto fresh = [&] {
            auto session = make_session(modules, config);
            expect(session.scheduler->register_binding("Root_Executed", [root_executions](auth::BindingContext&, const auth::ValueList&) {
                ++*root_executions; return eawr::core::Result<auth::ValueList>::success({});
            }).has_value(), "root execution observer");
            return session;
        };
        auto query = fresh();
        expect(query.scheduler->create_instance(1, "Query.lua").has_value(), "persistent query instance");
        const auto invoke = [&](Session& session, std::uint64_t frame) {
            auth::ScriptEvent event; event.kind = auth::ScriptEvent::Kind::call;
            event.key = {session.scheduler->completed_tick() + 1, 16, 1, 0}; event.target = 1; event.name = "Query";
            expect(session.scheduler->submit_event(event).has_value(), "persistent query event");
            auth::ServiceOptions options; options.time_tick = frame;
            auto report = session.scheduler->service(executor, options);
            expect(report && report.value().commands.size() == 1, "persistent query result");
            return report && !report.value().commands.empty() ? auth::test::text(report.value().commands.front()) : std::string{};
        };
        expect(invoke(query, 9000) == "1#0 report 1 9000", "sparse query before save");
        const auto bytes = query.scheduler->save().value();
        auto resumed = fresh();
        expect(resumed.scheduler->load(bytes).has_value() && resumed.scheduler->save().value() == bytes,
            "query load reconstructs Lua without root execution and re-saves exact bytes");
        // Host inspection interns its key under the sandbox's logical memory
        // accounting. Give both trajectories the same inspection inputs.
        const auto original_roots = query.scheduler->read_global(1, "Roots");
        const auto restored_roots = resumed.scheduler->read_global(1, "Roots");
        expect(*root_executions == 1 && std::get<LuaNumber>(original_roots.value()->data) == LuaNumber(1)
            && std::get<LuaNumber>(restored_roots.value()->data) == LuaNumber(1),
            "load executes neither a root callback nor its initializer");
        const auto with_clock_bound = [&](std::uint64_t bound) {
            std::string patched = bytes, marker(8, '\0');
            for (unsigned byte = 0; byte < 8; ++byte) marker[byte] = static_cast<char>((std::uint64_t{9000} >> (8U * byte)) & 255U);
            // This fixture's first integer 9000 is the upstream clock header,
            // before the same value in its instance. Lua numbers use b64 words.
            const auto offset = patched.find(marker);
            expect(offset != std::string::npos, "query clock header located");
            if (offset != std::string::npos) for (unsigned byte = 0; byte < 8; ++byte)
                patched[offset + byte] = static_cast<char>((bound >> (8U * byte)) & 255U);
            return patched;
        };
        const auto before_bad_load = resumed.scheduler->state_hash().value();
        expect(!resumed.scheduler->load(with_clock_bound(8999)) && resumed.scheduler->state_hash().value() == before_bad_load,
            "instance time above the saved query bound refuses transactionally");
        expect(!resumed.scheduler->load(with_clock_bound(auth::max_script_tick)) && resumed.scheduler->state_hash().value() == before_bad_load,
            "out-of-format saved query bound refuses transactionally");
        auth::ServiceOptions backwards; backwards.time_tick = 8999;
        const auto before_backwards = resumed.scheduler->state_hash().value();
        expect(!resumed.scheduler->service(executor, backwards) && resumed.scheduler->state_hash().value() == before_backwards,
            "loaded query clock retains its monotonic bound");
        expect(query.scheduler->state_hash().value() == resumed.scheduler->state_hash().value(),
            "matching host inspections preserve equal state before query continuation");
        const auto uninterrupted = invoke(query, 27000); const auto continued = invoke(resumed, 27000);
        expect(uninterrupted == "1#1 report 2 27000" && continued == "1#1 report 2 27000",
            "sparse query continuation preserves state and independent mode time");
        const auto digest = query.scheduler->state_hash().value();
        expect(resumed.scheduler->state_hash().value() == digest, "query continuation hash equals uninterrupted run");
        if (query_reference.empty()) query_reference = digest;
        else expect(query_reference == digest, "query persistence equals on 1/2/4/8 workers");
        auto normal = make_session(modules);
        expect(!normal.scheduler->load(bytes), "normal profile rejects query save identity");
        auto galactic_config = default_config(); galactic_config.galactic_clock = true;
        auto galactic = make_session(modules, galactic_config);
        expect(!galactic.scheduler->load(bytes), "galactic profile rejects query save identity");
        expect(!query.scheduler->load(normal.scheduler->save().value()), "query profile rejects normal save identity");
        auto timer = fresh();
        expect(timer.scheduler->create_instance(1, "Timer.lua").has_value(), "query timer instance");
        auth::ScriptEvent start; start.kind = auth::ScriptEvent::Kind::call;
        start.key = {1, 16, 1, 0}; start.target = 1; start.name = "Start";
        expect(timer.scheduler->submit_event(start).has_value(), "start query timer");
        auth::ServiceOptions time; time.time_tick = 0;
        expect(timer.scheduler->service(executor, time).has_value(), "query timer starts at upstream zero");
        const auto timer_bytes = timer.scheduler->save().value();
        auto timer_resumed = fresh();
        expect(timer_resumed.scheduler->load(timer_bytes).has_value() && timer_resumed.scheduler->save().value() == timer_bytes,
            "pending upstream-time timer survives exact save/load");
        time.time_tick = 90000;
        const auto fire = timer.scheduler->service(executor, time); const auto restored_fire = timer_resumed.scheduler->service(executor, time);
        expect(fire && restored_fire && fire.value().commands.size() == 1 && restored_fire.value().commands.size() == 1
            && auth::test::text(fire.value().commands.front()) == "1#0 report fired 90000"
            && auth::test::text(restored_fire.value().commands.front()) == "1#0 report fired 90000",
            "one-second timer fires after sparse upstream jump rather than thirty invocations");
        expect(timer.scheduler->state_hash().value() == timer_resumed.scheduler->state_hash().value(), "timer continuation matches uninterrupted state");
    }
    pinned_manifest_identity();
    const eawr::sim::InlineExecutor inline_executor;
    const Run reference = reference_run(inline_executor);
    std::size_t lines = 0;
    for (const std::string& tick : reference.ticks) lines += static_cast<std::size_t>(std::count(tick.begin(), tick.end(), '\n'));
    expect(lines > 300, "the scenario produces output");
    std::cout << "reference: " << lines << " lines, final state hash " << reference.hashes.back() << '\n';
    for (const std::string_view needle : {"guarded attempt to yield across", "EAWR-SCRIPT-0211", "R 12", "short done", "timer once",
                                          "doomed alive 5", "signal 5"}) {
        bool found = false;
        for (const std::string& tick : reference.ticks) found = found || tick.find(needle) != std::string::npos;
        expect(found, "reference output contains '" + std::string(needle) + "'");
    }
    if (!golden_final_hash.empty()) expect(reference.hashes.back() == golden_final_hash, "golden final state hash");
    // Hashes of distinct barriers differ; a rerun reproduces them.
    expect(reference.hashes[3] != reference.hashes[4], "state hash changes with the state");
    const Run again = reference_run(inline_executor);
    expect(again.hashes == reference.hashes && again.ticks == reference.ticks, "reference run is reproducible");

    for (const int saved : {0, 1, 2, 3, 4, 5, 7, 9, 10, 13, 21, 29}) {
        const std::string label = "save at " + std::to_string(saved);
        const std::string bytes = save_at(saved, inline_executor);
        Session session = loaded(bytes);
        expect(state_hash(session) == reference.hashes[static_cast<std::size_t>(saved)], label + ": loaded state hash");
        auto resaved = session.scheduler->save();
        expect(resaved.has_value() && resaved.value() == bytes, label + ": re-saving a loaded state gives the same bytes");
        expect(session.scheduler->completed_tick() == static_cast<std::uint64_t>(saved), label + ": completed tick");
        Run continued;
        continue_run(session, inline_executor, saved + 1, continued);
        compare_continuation(reference, continued, saved, label);
        if (saved == 2) {
            expect(!continued.ticks.empty() && continued.ticks.front().find("report again") != std::string::npos,
                   "a true-returned coroutine restarts after loading its live slot");
        }
    }
    // Loading over a running session replaces its state.
    {
        Session session = persistence_session();
        barrier_actions(session, 0);
        for (int tick = 1; tick <= 6; ++tick) {
            static_cast<void>(service_tick(session, inline_executor));
            barrier_actions(session, tick);
        }
        expect(session.scheduler->load(save_at(13, inline_executor)).has_value(), "load over a running session");
        Run continued;
        continue_run(session, inline_executor, 14, continued);
        compare_continuation(reference, continued, 13, "load over a running session");
    }
}

// ---- workers ----

void run_workers() {
    const eawr::sim::InlineExecutor inline_executor;
    const Run reference = reference_run(inline_executor);
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const std::string label = std::to_string(workers) + " workers";
        const Run run = reference_run(executor);
        expect(run.hashes == reference.hashes && run.ticks == reference.ticks, label + ": uninterrupted run");
        for (const int saved : {2, 10, 17}) {
            const std::string bytes = save_at(saved, executor);
            expect(bytes == save_at(saved, inline_executor), label + ": save bytes at " + std::to_string(saved));
            Session session = loaded(bytes);
            Run continued;
            continue_run(session, executor, saved + 1, continued);
            compare_continuation(reference, continued, saved, label + ", save at " + std::to_string(saved));
        }
    }
    // The #247 load scenario: 24 instances, saved halfway and continued on four workers.
    const auto make = [] {
        return make_session({{"Library/Library.lua", auth::test::scenario_library}, {"Ai.lua", auth::test::scenario_ai}});
    };
    const auto drive = [](Session& session, const eawr::sim::PartitionExecutor& executor, int first, int last,
                          std::vector<std::string>& hashes) {
        for (int tick = first; tick <= last; ++tick) {
            for (std::uint64_t id = 3; id <= 72; id += 3) {
                if (tick % 7 != static_cast<int>(id % 7)) continue;
                auth::ValueList arguments{auth::Value{auth::Handle{7, id % 24}}, auth::Value::number(integer(tick))};
                expect(session.scheduler
                           ->submit_event(dispatch_event({static_cast<std::uint64_t>(tick), 30, id, static_cast<std::uint64_t>(tick) * 100 + id},
                                                         id, "attacked", arguments))
                           .has_value(),
                       "scenario submit");
            }
            step(session, executor);
            hashes.push_back(state_hash(session));
        }
    };
    Session full = make();
    for (std::uint64_t id = 3; id <= 72; id += 3) expect(full.scheduler->create_instance(id, "Ai.lua").has_value(), "scenario create");
    std::vector<std::string> full_hashes;
    drive(full, inline_executor, 1, 90, full_hashes);
    Session first = make();
    for (std::uint64_t id = 3; id <= 72; id += 3) expect(first.scheduler->create_instance(id, "Ai.lua").has_value(), "scenario create");
    std::vector<std::string> hashes;
    drive(first, inline_executor, 1, 45, hashes);
    auto bytes = first.scheduler->save();
    expect(bytes.has_value(), "scenario save");
    // Separate sandboxes can be captured in parallel; the public save bytes
    // and state hash still match the serial writer, including suspended threads.
    struct SaveExecutor final : eawr::sim::PartitionExecutor {
        const eawr::sim::PartitionExecutor& delegate;
        mutable std::size_t save_phases{};
        mutable std::size_t save_partitions{};
        explicit SaveExecutor(const eawr::sim::PartitionExecutor& source) : delegate(source) {}
        std::size_t worker_count() const noexcept override { return delegate.worker_count(); }
        eawr::core::Result<void> execute(std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            return delegate.execute(count, partition);
        }
        eawr::core::Result<void> execute_phase(std::string_view name, std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            if (name == "script-save") { ++save_phases; save_partitions += count; }
            return delegate.execute_phase(name, count, partition);
        }
    };
    const auto serial_hash = first.scheduler->state_hash();
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        const SaveExecutor capture(pool);
        const auto parallel_bytes = first.scheduler->save(capture);
        const auto parallel_hash = first.scheduler->state_hash(capture);
        expect(bytes && parallel_bytes && bytes.value() == parallel_bytes.value(),
            "24-instance parallel save: byte-identical on " + std::to_string(workers) + " workers");
        expect(serial_hash && parallel_hash && serial_hash.value() == parallel_hash.value(),
            "24-instance parallel save: hash-identical on " + std::to_string(workers) + " workers");
        expect(capture.save_phases == (workers == 1 ? 0U : 2U)
            && capture.save_partitions == (workers == 1 ? 0U : 2U * eawr::sim::tick_partition_count),
            "24-instance parallel save: one partitioned phase per capture");
    }
    struct RefusingExecutor final : eawr::sim::PartitionExecutor {
        std::size_t worker_count() const noexcept override { return 8; }
        eawr::core::Result<void> execute(std::size_t,
            const std::function<void(std::size_t)>&) const override {
            eawr::core::Diagnostic reason;
            reason.code = auth::codes::invalid_request;
            reason.message = "capture executor failed";
            return eawr::core::Result<void>::failure(std::move(reason));
        }
    };
    const auto refused = first.scheduler->save(RefusingExecutor{});
    expect(!refused && refused.error().message == "capture executor failed",
        "parallel save propagates executor refusal");
    const auto unchanged = first.scheduler->save();
    expect(bytes && unchanged && bytes.value() == unchanged.value(),
        "executor refusal leaves the scheduler unchanged");
    Session second = make();
    expect(bytes.has_value() && second.scheduler->load(bytes.value()).has_value(), "scenario load");
    const eawr::platform::ThreadWorkerAdapter executor(4);
    drive(second, executor, 46, 90, hashes);
    expect(hashes == full_hashes, "scenario: save at 45, continue on 4 workers");
    second.commands.insert(second.commands.begin(), first.commands.begin(), first.commands.end());
    expect(second.commands == full.commands, "scenario: commands");
    std::cout << "scenario save: " << (bytes ? bytes.value().size() : 0) << " bytes for 24 instances\n";
}

// ---- corruption ----

std::uint64_t mix(std::uint64_t& state) {
    std::uint64_t value = (state += 0x9E3779B97F4A7C15ULL);
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

void run_corruption(std::uint64_t seed, int rounds) {
    const eawr::sim::InlineExecutor inline_executor;
    const Run reference = reference_run(inline_executor);
    const std::string source = save_at(11, inline_executor);
    const std::string pristine = save_at(7, inline_executor);
    Session target = loaded(pristine);
    const std::string target_hash = state_hash(target);
    expect(target_hash == reference.hashes[7], "target at barrier 7");

    std::size_t rejected = 0;
    std::size_t accepted = 0;
    const auto attempt = [&](const std::string& bytes, const std::string& label) {
        if (trace_attempts) std::cerr << label << std::endl;
        auto result = target.scheduler->load(bytes);
        if (!result) {
            ++rejected;
            expect(result.error().code == auth::codes::load_rejected, label + ": rejection code " + result.error().code);
            if (state_hash(target) != target_hash) {
                expect(false, label + ": a rejected load changed the scheduler");
                target = loaded(pristine);
            }
            return;
        }
        // A mutation can decode to another valid state: it must stay usable.
        ++accepted;
        auto saved = target.scheduler->save();
        if (saved) {
            Session reloaded = persistence_session();
            auto reload = reloaded.scheduler->load(saved.value());
            expect(reload.has_value(), label + ": accepted state re-loads: " + (reload ? std::string() : reload.error().message));
        }
        for (int tick = 0; tick < 3; ++tick) {
            if (!target.scheduler->service(inline_executor)) break;
        }
        expect(target.scheduler->load(pristine).has_value(), label + ": reset");
    };
    for (std::size_t size = 0; size < source.size(); size += size < 512 ? 1 : 61) {
        attempt(source.substr(0, size), "truncated to " + std::to_string(size));
    }
    attempt(source + '\0', "trailing byte");
    std::uint64_t random = seed;
    for (int round = 0; round < rounds; ++round) {
        std::string bytes = source;
        std::string label = "mutation round " + std::to_string(round) + ":";
        const int flips = 1 + static_cast<int>(mix(random) % 3);
        for (int flip = 0; flip < flips; ++flip) {
            const std::size_t position = static_cast<std::size_t>(mix(random) % bytes.size());
            bytes[position] = static_cast<char>(bytes[position] ^ static_cast<char>(1 + mix(random) % 255));
            label += " byte " + std::to_string(position);
        }
        attempt(bytes, label);
    }
    std::cout << "corruption: " << rejected << " rejected, " << accepted << " accepted as other valid states\n";
    expect(rejected > static_cast<std::size_t>(rounds) / 2, "corrupted saves are rejected");
    // After every rejection the target still continues exactly like the reference.
    expect(state_hash(target) == target_hash, "target unchanged after the corruption rounds");
    Run continued;
    continue_run(target, inline_executor, 8, continued);
    compare_continuation(reference, continued, 7, "target after corruption rounds");
}

// ---- rejects ----

// Replaces the only occurrence of a little-endian u32 in a save.
bool patch_u32(std::string& bytes, std::uint32_t from, std::uint32_t to) {
    char needle[4];
    char replacement[4];
    for (int index = 0; index < 4; ++index) {
        needle[index] = static_cast<char>((from >> (8 * index)) & 0xFF);
        replacement[index] = static_cast<char>((to >> (8 * index)) & 0xFF);
    }
    const std::string_view pattern(needle, 4);
    const std::size_t found = bytes.find(pattern);
    if (found == std::string::npos || bytes.find(pattern, found + 1) != std::string::npos) return false;
    bytes.replace(found, 4, replacement, 4);
    return true;
}

void expect_rejected(Session& session, const std::string& bytes, std::string_view reason, const std::string& label) {
    const std::string before = state_hash(session);
    auto result = session.scheduler->load(bytes);
    expect(!result && result.error().code == auth::codes::load_rejected &&
               result.error().message.find(reason) != std::string::npos,
           label + ": " + (result ? std::string("accepted") : result.error().message));
    expect(state_hash(session) == before, label + ": scheduler unchanged");
}

void run_rejects() {
    const eawr::sim::InlineExecutor inline_executor;
    const std::string bytes = save_at(8, inline_executor);
    run_inflated_counts(bytes);
    run_value_depth(inline_executor);
    {
        Session other = make_session({{"Persist.lua", std::string(persist_main) + "\n-- edited\n"},
                                      {"Library/Kit.lua", persist_kit},
                                      {"Exit.lua", persist_exit}},
                                     persistence_config());
        static_cast<void>(other.scheduler->register_binding("Test_Self", [](auth::BindingContext&, const auth::ValueList&) {
            return eawr::core::Result<auth::ValueList>::success({});
        }));
        expect_rejected(other, bytes, "other modules", "edited module");
    }
    {
        auth::SessionConfig config = persistence_config();
        config.seed += 1;
        Session other = persistence_session(config);
        expect_rejected(other, bytes, "other settings", "other seed");
    }
    {
        Session other = make_session({{"Persist.lua", persist_main}, {"Library/Kit.lua", persist_kit}, {"Exit.lua", persist_exit}},
                                     persistence_config());
        expect_rejected(other, bytes, "other bindings", "missing binding");
    }
    {
        Session other = persistence_session();
        expect_rejected(other, bytes.substr(0, bytes.size() - 1), "", "truncated");
        expect_rejected(other, bytes + "x", "trailing", "trailing bytes");
        expect_rejected(other, "", "", "empty");
    }
    // Quotas: a save whose content exceeds the session quotas fails even when
    // its header matches (the header is patched to the tighter session).
    const auto quota_save = [&](auth::Quotas quotas) {
        auth::SessionConfig config = persistence_config();
        config.quotas = quotas;
        Session session = persistence_session(config);
        barrier_actions(session, 0);
        for (int tick = 1; tick <= 8; ++tick) {
            static_cast<void>(service_tick(session, inline_executor));
            barrier_actions(session, tick);
        }
        auto saved = session.scheduler->save();
        expect(saved.has_value(), "quota save");
        return saved ? saved.value() : std::string();
    };
    {
        auth::Quotas quotas;
        quotas.thread_slots = 0x00C0FFEE;
        std::string patched = quota_save(quotas);
        expect(patch_u32(patched, 0x00C0FFEE, 3), "patch thread slot quota");
        quotas.thread_slots = 3;
        auth::SessionConfig config = persistence_config();
        config.quotas = quotas;
        Session other = persistence_session(config);
        expect_rejected(other, patched, "thread slot quota", "thread slot quota");
    }
    {
        auth::Quotas quotas;
        quotas.registrations = 0x00BEEF01;
        std::string patched = quota_save(quotas);
        expect(patch_u32(patched, 0x00BEEF01, 1), "patch registration quota");
        quotas.registrations = 1;
        auth::SessionConfig config = persistence_config();
        config.quotas = quotas;
        Session other = persistence_session(config);
        expect_rejected(other, patched, "quota exceeded", "registration quota");
    }
    {
        auth::Quotas quotas;
        quotas.identities = 0x00FACE01;
        std::string patched = quota_save(quotas);
        expect(patch_u32(patched, 0x00FACE01, 4), "patch identity quota");
        quotas.identities = 4;
        auth::SessionConfig config = persistence_config();
        config.quotas = quotas;
        Session other = persistence_session(config);
        expect_rejected(other, patched, "identity count or quota", "identity quota");
    }
    // An aborted session refuses to save.
    {
        Session session = script_session("function Main() Test_Throw() end\nCreate_Thread(\"Main\")");
        static_cast<void>(session.scheduler->register_binding("Test_Throw", [](auth::BindingContext&, const auth::ValueList& arguments) {
            static_cast<void>(std::get<std::string>(arguments.at(0).data));
            return eawr::core::Result<auth::ValueList>::success({});
        }));
        expect(session.scheduler->create_instance(1, "Main.lua").has_value(), "create throwing instance");
        expect(!session.scheduler->service(inline_executor).has_value(), "host exception aborts the session");
        auto saved = session.scheduler->save();
        expect(!saved && saved.error().code == auth::codes::session_abort, "an aborted session does not save");
    }
    // The combined authoritative hash is domain separated from both inputs.
    const std::string world(64, 'a');
    const std::string script(64, 'b');
    const std::string combined = auth::authoritative_state_sha256(7, world, script);
    expect(combined.size() == 64 && combined != auth::authoritative_state_sha256(8, world, script) &&
               combined != auth::authoritative_state_sha256(7, script, world),
           "authoritative state hash");
}

} // namespace

using namespace lua_persistence_test_support;

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "roundtrip") {
        run_roundtrip();
    } else if (mode == "workers") {
        run_workers();
    } else if (mode == "corruption") {
        // corruption [trace] [seed rounds]: CTest runs the default 3,000 rounds.
        std::vector<std::string_view> options(argv + 2, argv + argc);
        trace_attempts = !options.empty() && options.front() == "trace";
        if (trace_attempts) options.erase(options.begin());
        const std::uint64_t seed = options.size() == 2 ? std::stoull(std::string(options[0])) : 248;
        const int rounds = options.size() == 2 ? std::stoi(std::string(options[1])) : 3000;
        run_corruption(seed, rounds);
    } else if (mode == "rejects") {
        run_rejects();
    } else if (mode == "dump") {
        // The reference run's output, for reading the scenario.
        const eawr::sim::InlineExecutor inline_executor;
        const Run reference = reference_run(inline_executor);
        for (std::size_t tick = 0; tick < reference.ticks.size(); ++tick) {
            std::cout << "-- tick " << tick + 1 << ' ' << reference.hashes[tick + 1] << '\n' << reference.ticks[tick];
        }
        // `dump <tick> <file>` also writes the save at that barrier.
        if (argc > 3) {
            const std::string bytes = save_at(std::stoi(argv[2]), inline_executor);
            std::ofstream(argv[3], std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
    } else {
        std::cerr << "usage: lua_persistence_tests roundtrip | workers | corruption | rejects | dump\n";
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "passed\n";
    return 0;
}
