// PF particle spread (E): spread_ordered runs the pure part of a per-item loop on the particle workers and commits in item
// order on the calling thread. These contracts hold it to the plain serial loop at 1/2/4/8 workers.
#include "render_test_support.hpp"

#include "eawr/presentation/particles/ordered_spread.hpp"

#include <array>
#include <chrono>
#include <stdexcept>

namespace particle_render_contracts {
namespace {

// Real threads claiming tasks from a shared counter, the calling thread included, as the viewer's pool does.
class SpreadExecutor final : public particles::StepExecutor {
public:
    explicit SpreadExecutor(const std::size_t threads) : threads_(threads) {}
    [[nodiscard]] std::size_t participants() const noexcept override { return threads_; }
    bool run(const std::size_t count, const std::function<void(std::size_t)>& task) const override {
        std::atomic<std::size_t> next{0};
        const auto work = [&] {
            for (std::size_t index = next.fetch_add(1); index < count; index = next.fetch_add(1)) task(index);
        };
        std::vector<std::thread> helpers;
        for (std::size_t helper = 1; helper < threads_; ++helper) helpers.emplace_back(work);
        work();
        for (std::thread& helper : helpers) helper.join();
        return true;
    }

private:
    std::size_t threads_;
};

// A model of the emitters' per-ship loop: item i's prepare reads only the item's own input and writes only the item's own
// scratch; its commit writes the shared, order-sensitive state (a running hash of everything committed, a spawn counter
// that hands out ids, a log), as the registry's spawn, release and set_frame do.
struct Model final {
    explicit Model(const std::size_t count) : input(count), scratch(count), prepared(count) {
        for (std::size_t index = 0; index < count; ++index) input[index] = static_cast<std::uint64_t>(index) * 2654435761U + 17U;
    }
    std::vector<std::uint64_t> input;
    std::vector<std::uint64_t> scratch;
    std::vector<std::atomic<int>> prepared;
    std::uint64_t digest{1469598103934665603ULL};
    std::uint64_t next_id{1};
    std::vector<std::uint64_t> ids;
    std::vector<std::size_t> order;

    void prepare(const std::size_t index) {
        std::uint64_t value = input[index];
        for (int round = 0; round < 40; ++round) value = value * 6364136223846793005ULL + 1442695040888963407ULL;
        scratch[index] = value;
        prepared[index].fetch_add(1);
    }
    void commit(const std::size_t index) {
        order.push_back(index);
        // Even items "spawn": the id each gets depends on how many spawned before it.
        if ((scratch[index] & 1U) == 0U) ids.push_back(next_id++);
        digest = (digest ^ scratch[index]) * 1099511628211ULL;
    }
};

void spread_matches_the_serial_loop() {
    const SpreadExecutor one(1), two(2), four(4), eight(8);
    const std::array<const particles::StepExecutor*, 5> executors{nullptr, &one, &two, &four, &eight};
    for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{31}, std::size_t{32},
                                    std::size_t{33}, std::size_t{400}, std::size_t{5000}}) {
        Model serial(count);
        for (std::size_t index = 0; index < count; ++index) {
            serial.prepare(index);
            serial.commit(index);
        }
        for (const auto* executor : executors) {
            for (const std::size_t min_slice : {std::size_t{1}, std::size_t{8}, std::size_t{16}, std::size_t{100000}}) {
                Model model(count);
                const auto caller = std::this_thread::get_id();
                bool commits_on_caller = true, commit_after_prepare = true;
                std::size_t slices = 99;
                const bool ran = particles::spread_ordered(
                    executor, count, min_slice,
                    [&](const std::size_t index) {
                        if (index % 5 == 0) std::this_thread::sleep_for(std::chrono::microseconds(30));
                        model.prepare(index);
                    },
                    [&](const std::size_t index) {
                        if (std::this_thread::get_id() != caller) commits_on_caller = false;
                        if (model.prepared[index].load() != 1) commit_after_prepare = false;
                        model.commit(index);
                    },
                    &slices);
                expect(ran, "a spread over a working executor runs");
                expect(commits_on_caller && commit_after_prepare, "every commit runs on the caller, after its own prepare");
                expect(model.order.size() == count, "every item commits once");
                bool in_order = true;
                for (std::size_t index = 0; index < model.order.size(); ++index) in_order = in_order && model.order[index] == index;
                expect(in_order, "the commits run in item order");
                for (std::size_t index = 0; index < count; ++index) expect(model.prepared[index].load() == 1, "every item prepares once");
                expect(model.digest == serial.digest && model.ids == serial.ids && model.next_id == serial.next_id,
                       "the ordered commit state equals the serial loop's at any worker count and slice size");
                expect(slices <= particles::StepExecutor::max_ordered, "the slices stay within the ordered run's limit");
                if (executor == nullptr || executor->participants() < 2 || count < 2 * min_slice)
                    expect(slices == 0, "a small or executor-less spread runs inline");
            }
        }
    }
}

// An item that is stopped (a failing prepare) fails the run, and the commits before its slice stand, in order.
void spread_failure_keeps_a_prefix() {
    const SpreadExecutor four(4);
    Model model(400);
    const bool ran = particles::spread_ordered(
        &four, 400, 8,
        [&](const std::size_t index) {
            if (index == 250) throw std::runtime_error("prepare failure");
            model.prepare(index);
        },
        [&](const std::size_t index) { model.commit(index); });
    expect(!ran, "a prepare that throws fails the spread");
    bool prefix = model.order.size() <= 250;
    for (std::size_t index = 0; index < model.order.size(); ++index) prefix = prefix && model.order[index] == index;
    expect(prefix, "the commits before the failed item stand, in order, and none follows it");
}

// A commit that reads what an earlier commit wrote (the spawn order) sees it whatever runs the prepares.
void spread_commit_chain_sees_earlier_commits() {
    const SpreadExecutor eight(8);
    for (int repeat = 0; repeat < 20; ++repeat) {
        std::vector<std::uint64_t> chain;
        const bool ran = particles::spread_ordered(
            &eight, 600, 4, [](std::size_t) {},
            [&](const std::size_t index) { chain.push_back(chain.empty() ? index : chain.back() + index); });
        expect(ran && chain.size() == 600, "the chain commits every item");
        std::uint64_t expected = 0;
        bool same = true;
        for (std::size_t index = 0; index < chain.size(); ++index) {
            expected += index;
            same = same && chain[index] == expected;
        }
        expect(same, "each commit sees every earlier commit of the spread");
    }
}

} // namespace

void test_ordered_spread() {
    spread_matches_the_serial_loop();
    spread_failure_keeps_a_prefix();
    spread_commit_chain_sees_earlier_commits();
}

} // namespace particle_render_contracts
