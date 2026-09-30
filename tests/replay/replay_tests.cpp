#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/world.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

int failures = 0;

class FailingExecutor final : public eawr::sim::PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 4; }

    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t,
        const std::function<void(std::size_t)>&) const override {
        return eawr::core::Result<void>::failure(eawr::core::Diagnostic{
            .code = std::string(eawr::sim::diagnostic_codes::worker_failure),
            .severity = eawr::core::Severity::error,
            .message = "synthetic executor failure",
            .logical_path = std::nullopt,
            .line = std::nullopt,
            .column = std::nullopt,
            .source_id = std::string("replay-test"),
        });
    }
};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void put_u32(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (8U * index));
    }
}

void put_u64(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (8U * index));
    }
}

[[nodiscard]] eawr::sim::Replay load_fixture(const std::string& fixture_dir, const std::string& name) {
    const auto path = fixture_dir + "/" + name;
    const auto parsed = eawr::sim::parse_replay(read_bytes(path), path);
    expect(static_cast<bool>(parsed), "fixture parses: " + name);
    return parsed.value();
}

void test_sha256() {
    const std::vector<std::uint8_t> empty;
    expect(eawr::sim::sha256_hex(empty)
        == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "SHA-256 empty known vector");
    const std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
    expect(eawr::sim::sha256_hex(abc)
        == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256 abc known vector");
}

void test_parser_and_writer(const std::string& fixture_dir) {
    const auto bytes = read_bytes(fixture_dir + "/original-v1.eawr-replay");
    const auto parsed = eawr::sim::parse_replay(bytes, "original-v1.eawr-replay");
    expect(static_cast<bool>(parsed), "original fixture parses");
    if (parsed) {
        const auto encoded = eawr::sim::write_replay(parsed.value());
        expect(static_cast<bool>(encoded), "parsed replay writes");
        if (encoded) {
            expect(encoded.value() == bytes, "write(parse(fixture)) preserves exact frozen bytes");
        }
    }

    const std::array malformed{
        std::pair{"mutated-length-zero.eawr-replay", eawr::sim::diagnostic_codes::replay_malformed},
        std::pair{"mutated-version.eawr-replay", eawr::sim::diagnostic_codes::replay_version},
        std::pair{"mutated-order.eawr-replay", eawr::sim::diagnostic_codes::replay_order},
        std::pair{"mutated-count.eawr-replay", eawr::sim::diagnostic_codes::replay_malformed},
        std::pair{"mutated-trailing.eawr-replay", eawr::sim::diagnostic_codes::replay_malformed},
    };
    for (const auto& [name, code] : malformed) {
        const auto result = eawr::sim::parse_replay(read_bytes(fixture_dir + "/" + name), name);
        expect(!result, std::string("malformed fixture rejected: ") + name);
        if (!result) {
            expect(result.error().code == code, std::string("precise diagnostic class: ") + name);
            expect(result.error().message.find("command") != std::string::npos
                || result.error().message.find("version") != std::string::npos
                || result.error().message.find("trailing") != std::string::npos,
                std::string("diagnostic includes structural context: ") + name);
        }
    }
    auto truncated = bytes;
    truncated.pop_back();
    const auto truncated_result = eawr::sim::parse_replay(truncated, "truncated");
    expect(!truncated_result && truncated_result.error().message.find("length") != std::string::npos,
        "truncated command payload has a length diagnostic");

    auto excessive_count = bytes;
    put_u64(excessive_count, 48, eawr::sim::replay_max_entities + 1);
    const auto excessive_result = eawr::sim::parse_replay(excessive_count, "excessive-count");
    expect(!excessive_result
            && excessive_result.error().code == eawr::sim::diagnostic_codes::replay_resource_limit,
        "entity count resource limit is enforced before allocation");

    auto unknown_opcode = bytes;
    unknown_opcode[248] = 99;
    const auto opcode_result = eawr::sim::parse_replay(unknown_opcode, "unknown-opcode");
    expect(!opcode_result
            && opcode_result.error().code == eawr::sim::diagnostic_codes::replay_version,
        "unknown opcode is rejected as an unsupported contract");

    auto flags = bytes;
    flags[249] = 1;
    const auto flags_result = eawr::sim::parse_replay(flags, "flags");
    expect(!flags_result && flags_result.error().code == eawr::sim::diagnostic_codes::replay_version,
        "nonzero command flags are rejected");

    auto duplicate_key = bytes;
    put_u32(duplicate_key, 296, 1);
    const auto duplicate_key_result = eawr::sim::parse_replay(duplicate_key, "duplicate-key");
    expect(!duplicate_key_result
            && duplicate_key_result.error().code == eawr::sim::diagnostic_codes::replay_order,
        "duplicate command ordering key is rejected");

    auto duplicate_entity = bytes;
    put_u64(duplicate_entity, 160, 1);
    const auto duplicate_entity_result = eawr::sim::parse_replay(duplicate_entity, "duplicate-entity");
    expect(!duplicate_entity_result
            && duplicate_entity_result.error().code == eawr::sim::diagnostic_codes::replay_order,
        "duplicate initial stable ID is rejected");

    auto after_final = bytes;
    put_u64(after_final, 228, 5);
    const auto after_final_result = eawr::sim::parse_replay(after_final, "after-final");
    expect(!after_final_result
            && after_final_result.error().message.find("not less than final tick") != std::string::npos,
        "command at/after final tick is rejected with command context");

    auto nonreduced = bytes;
    put_u32(nonreduced, 24, 2);
    const auto nonreduced_result = eawr::sim::parse_replay(nonreduced, "nonreduced-rational");
    expect(!nonreduced_result
            && nonreduced_result.error().message.find("reduced rational") != std::string::npos,
        "non-reduced tick duration is rejected");
}

void test_oracle_states(const std::string& fixture_dir) {
    const auto replay = load_fixture(fixture_dir, "original-v1.eawr-replay");
    auto world_result = eawr::sim::World::create(replay);
    expect(static_cast<bool>(world_result), "world created from frozen fixture");
    auto world = std::move(world_result).value();
    const eawr::sim::InlineExecutor executor;
    constexpr std::array expected_hashes{
        "5f13df4359da99bbb781435f31303c6184510a08577f86868f58a02766c1da7e",
        "8a45cdf1c63c1e54b56b09a65dcaa1927adf83cad189592d1b24f5644499c98a",
        "f43e3626a0b5f1ea20aa8e5d53ccf9a12d125533a99e4139bf2d00822f92df54",
        "f99072695d44e55bcb3d02986787340221beac6fd6587c7b048f0ea71350eed9",
        "b716e13851efdfd2854dd16ddeefe14f5f031d81c2e8a927aa0109ef67e484bc",
    };
    constexpr std::array expected_next{4ULL, 5ULL, 6ULL, 6ULL, 7ULL};
    constexpr std::array expected_rng{
        0x9f5abf2108f64a04ULL, 0x3d9238da8840c619ULL, 0xdbc9b294078b422eULL,
        0x7a012c4d86d5be43ULL, 0x1838a60706203a58ULL,
    };
    constexpr std::array expected_nonce{
        0x157a3807a48faa9dULL, 0xd573529b34a1d093ULL, 0x2f90b72e996dccbeULL,
        0xa2d419334c4667ecULL, 0x01404ce914938008ULL,
    };
    constexpr std::array expected_ids{
        std::array{1ULL, 2ULL, 3ULL, 0ULL},
        std::array{1ULL, 3ULL, 4ULL, 0ULL},
        std::array{1ULL, 3ULL, 4ULL, 5ULL},
        std::array{4ULL, 5ULL, 0ULL, 0ULL},
        std::array{4ULL, 5ULL, 6ULL, 0ULL},
    };
    constexpr std::array expected_x{
        std::array{7LL, 22LL, 102LL, 0LL},
        std::array{5LL, 100LL, -18LL, 0LL},
        std::array{3LL, 502LL, -16LL, -102LL},
        std::array{-48LL, -104LL, 0LL, 0LL},
        std::array{-46LL, -106LL, 40LL, 0LL},
    };
    constexpr std::array expected_counts{3U, 3U, 4U, 2U, 3U};

    for (std::size_t tick = 0; tick < expected_hashes.size(); ++tick) {
        const auto result = world.step(executor);
        expect(static_cast<bool>(result), "oracle tick executes");
        if (!result) {
            break;
        }
        expect(result.value().state_sha256 == expected_hashes[tick], "frozen per-tick state SHA-256");
        expect(world.next_entity_id() == expected_next[tick], "frozen next stable ID");
        expect(world.rng_state() == expected_rng[tick], "frozen SplitMix64 state");
        expect(world.tick_nonce() == expected_nonce[tick], "frozen SplitMix64 nonce");
        const auto entities = world.entities();
        expect(entities.size() == expected_counts[tick], "frozen live entity count");
        for (std::size_t index = 0; index < entities.size(); ++index) {
            expect(entities[index].entity_id == expected_ids[tick][index], "frozen stable ID order");
            expect(entities[index].position.x.raw() == expected_x[tick][index], "frozen position state");
        }
    }
}

[[nodiscard]] std::vector<std::string> run_hashes(
    const eawr::sim::Replay& replay,
    const std::size_t workers,
    const bool scramble) {
    auto created = eawr::sim::World::create(replay);
    auto world = std::move(created).value();
    if (scramble) {
        world.scramble_storage_for_testing();
    }
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    std::vector<std::string> hashes;
    while (world.completed_tick() < world.final_tick_count()) {
        const auto result = world.step(executor);
        expect(static_cast<bool>(result), "threaded replay tick succeeds");
        if (!result) {
            break;
        }
        hashes.push_back(result.value().state_sha256);
        if (scramble) {
            world.scramble_storage_for_testing();
        }
    }
    return hashes;
}

void test_determinism_and_sensitivity(const std::string& fixture_dir) {
    const auto replay = load_fixture(fixture_dir, "original-v1.eawr-replay");
    const auto one = run_hashes(replay, 1, false);
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        expect(run_hashes(replay, workers, false) == one,
            std::to_string(workers) + " workers match one worker");
    }
    expect(run_hashes(replay, 3, false) == one, "three workers match one worker");
    expect(run_hashes(replay, 4, true) == one, "storage scrambling preserves every hash");

    auto changed_seed = replay;
    ++changed_seed.seed;
    expect(run_hashes(changed_seed, 1, false).front() != one.front(), "seed changes first state hash");
    auto changed_future = replay;
    auto& set_velocity = std::get<eawr::sim::SetVelocityCommand>(changed_future.commands.back().payload);
    set_velocity.velocity.x = eawr::sim::math::Fixed::from_raw(-46);
    expect(run_hashes(changed_future, 1, false).front() != one.front(),
        "future pending command changes earlier state hash");
}

void test_atomicity_and_snapshots(const std::string& fixture_dir) {
    const auto replay = load_fixture(fixture_dir, "mutated-overflow.eawr-replay");
    auto created = eawr::sim::World::create(replay);
    auto world = std::move(created).value();
    const auto before_hash = world.state_sha256();
    const auto before_entities = world.entities();
    const auto before_snapshot = world.snapshot();
    const eawr::platform::ThreadWorkerAdapter executor(4);
    const auto failed = world.step(executor);
    expect(!failed && failed.error().code == eawr::sim::diagnostic_codes::movement_overflow,
        "overflow tick fails with movement diagnostic");
    expect(world.completed_tick() == 0 && world.rng_state() == replay.seed && world.tick_nonce() == 0,
        "overflow preserves tick and RNG atomically");
    expect(world.entities() == before_entities && world.state_sha256() == before_hash,
        "overflow preserves entities and canonical state atomically");
    expect(world.snapshot() == before_snapshot && before_snapshot->completed_tick() == 0,
        "overflow publishes no replacement snapshot");

    auto executor_failure_created = eawr::sim::World::create(
        load_fixture(fixture_dir, "original-v1.eawr-replay"));
    auto executor_failure_world = std::move(executor_failure_created).value();
    const auto executor_failure_hash = executor_failure_world.state_sha256();
    const FailingExecutor failing_executor;
    const auto executor_failure = executor_failure_world.step(failing_executor);
    expect(!executor_failure
            && executor_failure.error().code == eawr::sim::diagnostic_codes::worker_failure,
        "external executor failure is propagated");
    expect(executor_failure_world.completed_tick() == 0
            && executor_failure_world.state_sha256() == executor_failure_hash,
        "external executor failure leaves the tick atomic");

    const auto maximum = std::numeric_limits<std::int64_t>::max();
    eawr::sim::Replay multiple_errors{
        1, 1, 0, 1, {},
        {
            {1, 1, {eawr::sim::math::Fixed::from_raw(maximum), {}, {}},
                {eawr::sim::math::Fixed::from_raw(1), {}, {}}},
            {2, 2, {eawr::sim::math::Fixed::from_raw(maximum), {}, {}},
                {eawr::sim::math::Fixed::from_raw(1), {}, {}}},
        },
        {},
    };
    auto multiple_created = eawr::sim::World::create(multiple_errors);
    auto multiple_world = std::move(multiple_created).value();
    const auto multiple_result = multiple_world.step(executor);
    expect(!multiple_result && multiple_result.error().message.find("entity 1 component 0")
            != std::string::npos,
        "parallel movement reports the smallest stable ID and first component error");

    const auto valid = load_fixture(fixture_dir, "original-v1.eawr-replay");
    auto valid_created = eawr::sim::World::create(valid);
    auto valid_world = std::move(valid_created).value();
    const eawr::sim::InlineExecutor inline_executor;
    const auto first = valid_world.step(inline_executor);
    const auto held = first.value().snapshot;
    const auto held_instances = std::vector<eawr::sim::RenderInstance>(
        held->instances().begin(), held->instances().end());
    const auto second = valid_world.step(inline_executor);
    expect(static_cast<bool>(second), "second snapshot tick succeeds");
    expect(held->completed_tick() == 1
        && std::vector<eawr::sim::RenderInstance>(held->instances().begin(), held->instances().end())
            == held_instances,
        "held snapshot remains an immutable isolated copy");
    expect(held != valid_world.snapshot(), "each successful tick publishes a new snapshot object");
    expect(held->instances()[0].fixed_transform.rows[0][3].raw() == 7,
        "snapshot fixed transform contains frozen position");
}

void test_command_errors_and_id_rules(const std::string& fixture_dir) {
    auto replay = load_fixture(fixture_dir, "original-v1.eawr-replay");
    replay.commands[0].payload = eawr::sim::DestroyCommand{999};
    auto created = eawr::sim::World::create(replay);
    auto world = std::move(created).value();
    const auto before = world.state_sha256();
    const eawr::sim::InlineExecutor executor;
    const auto failure = world.step(executor);
    expect(!failure && failure.error().code == eawr::sim::diagnostic_codes::invalid_command,
        "unknown entity command is rejected");
    expect(failure.error().message.find("command 0 at tick 0") != std::string::npos,
        "command error includes command and tick context");
    expect(world.completed_tick() == 0 && world.state_sha256() == before,
        "command failure is tick-atomic");

    eawr::sim::Replay no_reuse{
        1, 1, 0, 1, {},
        {eawr::sim::EntityState{1, 1, {}, {}}},
        {
            {{0, 1, 1}, eawr::sim::DestroyCommand{1}},
            {{0, 1, 2}, eawr::sim::CreateCommand{{1, 2, {}, {}}}},
        },
    };
    auto no_reuse_created = eawr::sim::World::create(no_reuse);
    auto no_reuse_world = std::move(no_reuse_created).value();
    const auto no_reuse_result = no_reuse_world.step(executor);
    expect(!no_reuse_result && no_reuse_result.error().message.find("next ID 2") != std::string::npos,
        "destroyed stable IDs cannot be reused");
    expect(no_reuse_world.entities().size() == 1 && no_reuse_world.entities()[0].entity_id == 1,
        "failed destroy/create sequence rolls back the destroy");

    const auto maximum = std::numeric_limits<eawr::sim::EntityId>::max();
    eawr::sim::Replay exhausted{
        1, 1, 0, 1, {},
        {eawr::sim::EntityState{maximum, 1, {}, {}}},
        {{{0, 1, 1}, eawr::sim::CreateCommand{{maximum, 2, {}, {}}}}},
    };
    auto exhausted_created = eawr::sim::World::create(exhausted);
    auto exhausted_world = std::move(exhausted_created).value();
    expect(exhausted_world.next_entity_id() == 0, "UINT64_MAX initial ID preserves exhausted next-ID zero");
    const auto exhausted_result = exhausted_world.step(executor);
    expect(!exhausted_result && exhausted_result.error().code == eawr::sim::diagnostic_codes::id_exhausted,
        "creation after ID exhaustion fails distinctly");
}

void test_actual_thread_adapter() {
    namespace platform = eawr::platform;
    const auto counts = platform::determinism_worker_counts();
    expect(std::is_sorted(counts.begin(), counts.end())
            && std::adjacent_find(counts.begin(), counts.end()) == counts.end()
            && std::find(counts.begin(), counts.end(), 8U) != counts.end()
            && std::find(counts.begin(), counts.end(), platform::ThreadWorkerAdapter::hardware_worker_count())
                != counts.end(),
        "determinism worker counts are 1, 2, 4, 8 and the hardware count, ascending");

    // One partition per worker, each blocking until all have arrived: every worker is its own
    // live thread, and the same threads serve every later phase (the pool is persistent).
    for (const auto worker_count : counts) {
        const platform::ThreadWorkerAdapter adapter(worker_count);
        std::condition_variable ready;
        std::mutex mutex;
        std::set<std::thread::id> thread_ids;
        const auto result = adapter.execute(worker_count, [&](const std::size_t) {
            std::unique_lock lock(mutex);
            thread_ids.insert(std::this_thread::get_id());
            if (thread_ids.size() == worker_count) {
                ready.notify_all();
            } else {
                ready.wait(lock, [&] { return thread_ids.size() == worker_count; });
            }
        });
        expect(static_cast<bool>(result), "thread adapter executes requested workers");
        expect(thread_ids.size() == worker_count, "thread adapter runs one partition per worker thread at once");
        const auto first_phase = thread_ids;
        for (int phase = 0; phase < 50; ++phase) {
            const auto again = adapter.execute(eawr::sim::tick_partition_count, [&](const std::size_t) {
                const std::lock_guard lock(mutex);
                thread_ids.insert(std::this_thread::get_id());
            });
            expect(static_cast<bool>(again), "the pool runs repeated phases");
        }
        expect(thread_ids == first_phase, "later phases reuse the pool's threads");
    }

    // Any partition count runs every partition exactly once, whatever the worker count.
    for (const std::size_t worker_count : {1U, 3U, 4U}) {
        const platform::ThreadWorkerAdapter adapter(worker_count);
        for (const std::size_t partitions : {0U, 1U, 5U, 64U, 1000U}) {
            std::vector<std::atomic<int>> runs(partitions);
            const auto result = adapter.execute(partitions, [&](const std::size_t partition) {
                runs[partition].fetch_add(1);
            });
            expect(static_cast<bool>(result)
                    && std::all_of(runs.begin(), runs.end(), [](const auto& count) { return count.load() == 1; }),
                std::to_string(partitions) + " partitions each run once on " + std::to_string(worker_count)
                    + " workers");
        }
    }

    // A throwing partition fails the phase with the worker diagnostic naming the lowest
    // partition that threw; the others still run and the pool stays usable.
    {
        const platform::ThreadWorkerAdapter adapter(4);
        std::atomic<int> completed{0};
        const auto thrown = adapter.execute(64, [&](const std::size_t partition) {
            if (partition == 41) {
                throw 7;
            }
            if (partition == 3 || partition == 17) {
                throw std::runtime_error("boom " + std::to_string(partition));
            }
            completed.fetch_add(1);
        });
        expect(!thrown && thrown.error().code == eawr::sim::diagnostic_codes::worker_failure
                && thrown.error().message == "partition 3 threw: boom 3",
            "a throwing partition reports the lowest partition that threw");
        expect(completed.load() == 61, "the other partitions of a failed phase still run");
        const auto unknown = adapter.execute(8, [](const std::size_t partition) {
            if (partition == 5) {
                throw 5;
            }
        });
        expect(!unknown && unknown.error().message == "partition 5 threw: unknown exception",
            "a non-standard exception is reported as unknown");
        std::atomic<int> after{0};
        expect(static_cast<bool>(adapter.execute(64, [&](const std::size_t) { after.fetch_add(1); }))
                && after.load() == 64,
            "the pool runs phases after a failed one");
    }

    // A partition that calls execute() on the same pool runs the inner phase inline.
    {
        const platform::ThreadWorkerAdapter adapter(4);
        std::atomic<int> inner{0};
        std::atomic<int> inner_failures{0};
        const auto nested = adapter.execute(8, [&](const std::size_t) {
            if (!adapter.execute(8, [&](const std::size_t) { inner.fetch_add(1); })) {
                inner_failures.fetch_add(1);
            }
        });
        expect(static_cast<bool>(nested) && inner_failures.load() == 0 && inner.load() == 64,
            "nested phases run every inner partition");
    }

    // Two threads sharing one pool take turns; each phase still runs completely.
    {
        const platform::ThreadWorkerAdapter adapter(4);
        std::atomic<int> total{0};
        std::atomic<int> failed{0};
        const auto caller = [&] {
            for (int phase = 0; phase < 200; ++phase) {
                if (!adapter.execute(16, [&](const std::size_t) { total.fetch_add(1); })) {
                    failed.fetch_add(1);
                }
            }
        };
        std::thread other(caller);
        caller();
        other.join();
        expect(failed.load() == 0 && total.load() == 2 * 200 * 16, "concurrent callers share one pool");
    }

    for (const std::size_t invalid : {std::size_t{0}, platform::ThreadWorkerAdapter::max_worker_count + 1}) {
        const platform::ThreadWorkerAdapter adapter(invalid);
        const auto result = adapter.execute(4, [](const std::size_t) {});
        expect(!result && result.error().code == eawr::sim::diagnostic_codes::worker_failure,
            "a pool of " + std::to_string(invalid) + " workers refuses to execute");
    }
}

// #637: Dispatch::by_cost keeps small named phases on the calling thread, hands a phase that
// outgrows the budget to the pool part way, and still runs every partition exactly once.
void test_dispatch_by_cost() {
    namespace platform = eawr::platform;
    using Adapter = platform::ThreadWorkerAdapter;
    const auto caller = std::this_thread::get_id();

    // Small phases: after the first, they stay on the calling thread in partition order. The
    // budget is a time, so a descheduled caller may hand a few to the pool; most stay inline.
    {
        const Adapter adapter(4, Adapter::Dispatch::by_cost);
        int off_caller = 0;
        bool ordered = true;
        for (int phase = 0; phase < 100; ++phase) {
            // One slot per partition, so the partitions never contend with each other.
            std::array<std::thread::id, 64> ran_on{};
            std::array<std::uint32_t, 64> sequence{};
            std::atomic<std::uint32_t> next{0};
            const auto result = adapter.execute_phase("small", 64, [&](const std::size_t partition) {
                ran_on[partition] = std::this_thread::get_id();
                sequence[partition] = next.fetch_add(1, std::memory_order_relaxed);
            });
            expect(static_cast<bool>(result) && next.load() == 64, "a small phase runs all 64 partitions");
            off_caller += static_cast<int>(std::count_if(ran_on.begin(), ran_on.end(), [&](const auto id) { return id != caller; }));
            const auto counts = adapter.phase_counts();
            if (counts.escalated_phases == 0 && counts.pool_phases == 0) {
                ordered = ordered && std::is_sorted(sequence.begin(), sequence.end());
            }
        }
        const auto counts = adapter.phase_counts();
        expect(counts.inline_phases + counts.escalated_phases + counts.pool_phases == 100,
            "every named phase is counted once");
        expect(counts.inline_phases >= 50, "small phases stay on the calling thread (" + std::to_string(counts.inline_phases)
                + " of 100 inline)");
        expect(ordered, "an inline phase runs its partitions in partition order");
        expect(counts.pool_phases > 0 || counts.escalated_phases > 0 || off_caller == 0,
            "only a pool phase runs partitions on other threads");
    }

    // A small phase that turns big hands its remaining partitions to the pool once the budget is
    // spent; the next run of that phase goes to the pool at once.
    {
        const Adapter adapter(4, Adapter::Dispatch::by_cost);
        expect(static_cast<bool>(adapter.execute_phase("burst", 64, [](const std::size_t) {})), "the burst phase starts small");
        std::vector<std::atomic<int>> runs(64);
        std::mutex mutex;
        std::set<std::thread::id> threads;
        const auto slow = [&](const std::size_t partition) {
            runs[partition].fetch_add(1);
            {
                const std::lock_guard lock(mutex);
                threads.insert(std::this_thread::get_id());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        const auto before = adapter.phase_counts();
        expect(static_cast<bool>(adapter.execute_phase("burst", 64, slow)), "the grown phase runs");
        const auto grown = adapter.phase_counts();
        expect(grown.escalated_phases == before.escalated_phases + 1, "a grown phase moves to the pool part way");
        expect(std::all_of(runs.begin(), runs.end(), [](const auto& count) { return count.load() == 1; }),
            "a phase handed to the pool part way runs each partition once");
        expect(threads.size() > 1, "the pool helps a grown phase");
        for (auto& count : runs) count.store(0);
        expect(static_cast<bool>(adapter.execute_phase("burst", 64, slow)), "the big phase runs again");
        const auto big = adapter.phase_counts();
        expect(big.pool_phases == grown.pool_phases + 1, "a phase that was big goes to the pool at once");
        expect(std::all_of(runs.begin(), runs.end(), [](const auto& count) { return count.load() == 1; }),
            "a pool phase runs each partition once");
    }

    // Failures and nesting behave as on the pool: the lowest throwing partition, inline or not.
    {
        const Adapter adapter(4, Adapter::Dispatch::by_cost);
        for (int phase = 0; phase < 3; ++phase) {
            std::atomic<int> completed{0};
            const auto thrown = adapter.execute_phase("throws", 64, [&](const std::size_t partition) {
                if (partition == 17 || partition == 40) throw std::runtime_error("boom " + std::to_string(partition));
                if (phase == 2) std::this_thread::sleep_for(std::chrono::microseconds(200));
                completed.fetch_add(1);
            });
            expect(!thrown && thrown.error().message == "partition 17 threw: boom 17" && completed.load() == 62,
                "a by-cost phase reports the lowest partition that threw and runs the others");
        }
        std::atomic<int> inner{0};
        const auto nested = adapter.execute_phase("outer", 8, [&](const std::size_t) {
            static_cast<void>(adapter.execute_phase("inner", 8, [&](const std::size_t) { inner.fetch_add(1); }));
        });
        expect(static_cast<bool>(nested) && inner.load() == 64, "a by-cost phase nests inline");
        std::atomic<int> unnamed{0};
        const auto before = adapter.phase_counts();
        expect(static_cast<bool>(adapter.execute(64, [&](const std::size_t) { unnamed.fetch_add(1); })) && unnamed.load() == 64,
            "an unnamed phase runs on the pool");
        const auto after = adapter.phase_counts();
        expect(after.inline_phases == before.inline_phases && after.pool_phases == before.pool_phases
                && after.escalated_phases == before.escalated_phases,
            "unnamed phases are not counted");
    }

    expect(Adapter::inline_budget(1) < Adapter::inline_budget(4) && Adapter::inline_budget(4) < Adapter::inline_budget(28),
        "the inline budget grows with the threads a phase wakes");
}

// #637: the state hasher returns sha256_hex of the bytes, in any number and order of waits,
// and resolves every hash it took before it is destroyed.
void test_state_hasher() {
    std::vector<std::vector<std::uint8_t>> inputs;
    for (std::size_t size : {0U, 1U, 55U, 64U, 1000U, 100'000U}) {
        std::vector<std::uint8_t> bytes(size);
        for (std::size_t index = 0; index < size; ++index) bytes[index] = static_cast<std::uint8_t>(index * 31 + size);
        inputs.push_back(std::move(bytes));
    }
    std::vector<eawr::sim::StateHash> hashes;
    {
        eawr::platform::ThreadStateHasher hasher(1);
        for (int round = 0; round < 20; ++round) {
            for (const auto& bytes : inputs) hashes.push_back(hasher.hash(bytes));
        }
        expect(hashes[3].get() == eawr::sim::sha256_hex(inputs[3]), "a pending hash resolves to sha256_hex");
    }
    bool equal = true;
    for (std::size_t index = 0; index < hashes.size(); ++index) {
        equal = equal && hashes[index].get() == eawr::sim::sha256_hex(inputs[index % inputs.size()]);
    }
    expect(equal, "every hash the hasher took resolves, also after it is gone");
    expect(eawr::sim::StateHash().get().empty() && eawr::sim::StateHash("ab").get() == "ab",
        "an empty hash is empty and a ready one is its value");
}

} // namespace

int main(const int argc, const char* const argv[]) {
    if (argc != 2) {
        std::cerr << "usage: replay_contract_tests <fixture-dir>\n";
        return 2;
    }
    test_sha256();
    test_parser_and_writer(argv[1]);
    test_oracle_states(argv[1]);
    test_determinism_and_sensitivity(argv[1]);
    test_atomicity_and_snapshots(argv[1]);
    test_command_errors_and_id_rules(argv[1]);
    test_actual_thread_adapter();
    test_dispatch_by_cost();
    test_state_hasher();
    if (failures != 0) {
        std::cerr << failures << " replay contract test(s) failed\n";
        return 1;
    }
    std::cout << "replay contracts passed\n";
    return 0;
}
