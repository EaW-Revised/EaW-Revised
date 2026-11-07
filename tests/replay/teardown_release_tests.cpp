// The tick's teardown phase (docs/simulation.md, `teardown`): the survivors' heap memory is freed in the fixed
// 64 index ranges instead of one serial pass. These checks count work, never time: every element is reset exactly
// once for 1, 2, 4 and 8 workers, the vectors keep their size and capacity, a small tick issues no phase, and the
// result does not depend on which thread ran a range.

#include "../support/check.hpp"
#include "../../src/sim/tactical/session_release.hpp"
#include "../../src/sim/tactical/session_types.hpp"

#include "eawr/platform/sim_workers.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;
namespace sd = eawr::sim::tactical::session_detail;
using eawr::sim::PartitionExecutor;
using eawr::sim::tick_partition_count;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
}

// Counts the executor's named phases; forwards the work to the wrapped executor.
class RecordingExecutor final : public PartitionExecutor {
public:
    explicit RecordingExecutor(const PartitionExecutor& inner) : inner_(inner) {}
    [[nodiscard]] std::size_t worker_count() const noexcept override { return inner_.worker_count(); }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t count, const std::function<void(std::size_t)>& partition) const override {
        return inner_.execute(count, partition);
    }
    [[nodiscard]] eawr::core::Result<void> execute_phase(
        const std::string_view phase, const std::size_t count, const std::function<void(std::size_t)>& partition) const override {
        const std::lock_guard lock(mutex_);
        phases_.emplace_back(std::string(phase), count);
        return inner_.execute_phase(phase, count, partition);
    }
    [[nodiscard]] std::vector<std::pair<std::string, std::size_t>> phases() const { return phases_; }

private:
    const PartitionExecutor& inner_;
    mutable std::mutex mutex_;
    mutable std::vector<std::pair<std::string, std::size_t>> phases_;
};

// An element that records each reset (a move assignment from a fresh element) and owns heap memory.
struct Counted {
    static inline std::atomic<std::size_t> resets{0};
    std::uint64_t value{};
    std::vector<std::uint64_t> heap;
    Counted() = default;
    Counted(const Counted&) = default;
    Counted(Counted&&) noexcept = default;
    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&& other) noexcept {
        resets.fetch_add(1, std::memory_order_relaxed);
        value = other.value;
        heap = std::move(other.heap);
        return *this;
    }
};

std::vector<Counted> filled(const std::size_t count, const std::uint64_t salt) {
    std::vector<Counted> values(count);
    for (std::size_t index = 0; index < count; ++index) {
        values[index].value = salt + index + 1;
        values[index].heap.assign(1 + index % 5, salt);
    }
    return values;
}

bool all_reset(const std::vector<Counted>& values) {
    for (const auto& value : values) if (value.value != 0 || !value.heap.empty()) return false;
    return true;
}

void test_every_element_once(const PartitionExecutor& executor, const std::size_t workers) {
    const std::string label = " (" + std::to_string(workers) + " workers)";
    RecordingExecutor recording(executor);
    // Sizes that do not divide by 64, and one empty vector in the list.
    auto first = filled(1001, 10);
    auto second = filled(777, 20);
    std::vector<Counted> empty;
    const auto first_capacity = first.capacity();
    Counted::resets = 0;
    std::array<std::vector<Counted>*, 3> list{&first, &empty, &second};
    const auto released = sd::release_elements<Counted>(recording, "teardown",
        std::span<std::vector<Counted>* const>(list.data(), list.size()));
    expect(released == 1001 + 777, "the work count is the number of elements" + label);
    expect(Counted::resets.load() == 1001 + 777, "every element is reset exactly once" + label);
    expect(first.size() == 1001 && second.size() == 777 && empty.empty(), "sizes are kept" + label);
    expect(first.capacity() == first_capacity, "capacity is kept" + label);
    expect(all_reset(first) && all_reset(second), "every element is back to its default" + label);
    const auto phases = recording.phases();
    expect(phases.size() == 1 && phases[0].first == "teardown" && phases[0].second == tick_partition_count,
        "one 64-partition teardown phase" + label);
}

void test_small_tick_issues_no_phase() {
    const eawr::sim::InlineExecutor inline_executor;
    RecordingExecutor recording(inline_executor);
    auto values = filled(sd::release_parallel_cutoff - 1, 3);
    Counted::resets = 0;
    std::array<std::vector<Counted>*, 1> list{&values};
    const auto released = sd::release_elements<Counted>(recording, "teardown",
        std::span<std::vector<Counted>* const>(list.data(), list.size()));
    expect(released == 0 && Counted::resets.load() == 0, "below the cutoff nothing is reset");
    expect(recording.phases().empty(), "below the cutoff no phase is issued");
    expect(values[0].value == 4 && !values[0].heap.empty(), "below the cutoff the elements are untouched");
    auto at_cutoff = filled(sd::release_parallel_cutoff, 3);
    list[0] = &at_cutoff;
    expect(sd::release_elements<Counted>(recording, "teardown", std::span<std::vector<Counted>* const>(list.data(), 1))
            == sd::release_parallel_cutoff,
        "at the cutoff every element is reset");
}

// The real element type: a unit that owns heap memory in several components ends up default-constructed.
void test_live_units(const PartitionExecutor& executor, const std::size_t workers) {
    const std::string label = " (" + std::to_string(workers) + " workers)";
    std::vector<sd::LiveUnit> units(600);
    for (std::size_t index = 0; index < units.size(); ++index) {
        auto& unit = units[index];
        unit.state.entity_id = 2 * (index + 1);
        { tactical::CarriedObject object; object.entity_id = index + 1; unit.state.contained.push_back(object); }
        unit.combat.emplace();
        unit.waypoints.emplace();
        unit.waypoints->points.emplace_back();
        unit.position_history.emplace();
    }
    std::array<std::vector<sd::LiveUnit>*, 1> list{&units};
    const auto released = sd::release_elements<sd::LiveUnit>(executor, "teardown",
        std::span<std::vector<sd::LiveUnit>* const>(list.data(), 1));
    expect(released == 600, "all live units are reset" + label);
    bool clean = units.size() == 600;
    for (const auto& unit : units) {
        clean = clean && unit.state.entity_id == 0 && unit.state.contained.empty() && !unit.combat
            && !unit.waypoints && !unit.position_history;
    }
    expect(clean, "a reset live unit is default-constructed" + label);
}

} // namespace

int main() {
    test_small_tick_issues_no_phase();
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        eawr::platform::ThreadWorkerAdapter pool(workers);
        test_every_element_once(pool, workers);
        test_live_units(pool, workers);
    }
    {
        eawr::sim::InlineExecutor inline_executor;
        test_every_element_once(inline_executor, 1);
        test_live_units(inline_executor, 1);
    }
    if (failures != 0) {
        std::cerr << failures << " teardown release check(s) failed\n";
        return 1;
    }
    std::cout << "teardown release contracts passed\n";
    return 0;
}
