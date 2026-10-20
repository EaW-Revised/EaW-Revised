#include "../../support/check.hpp"
#include <exception>

#include "eawr/presentation/space/event_snapshot_cache.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

namespace {
struct Snapshot final {
    std::uint64_t tick{};
    int value{};
    [[nodiscard]] std::uint64_t completed_tick() const noexcept { return tick; }
};

using eawr::testing::require;

void admitted_pair_needs_no_history() {
    const Snapshot previous{5999, 1}, latest{6000, 2};
    eawr::presentation::space::EventSnapshotCache cache(previous, latest);
    unsigned fetches = 0;
    const auto fetch = [&](std::uint64_t) -> std::shared_ptr<const Snapshot> { ++fetches; return {}; };
    for (unsigned event = 0; event < 4096; ++event) {
        require(cache.get(5999, fetch) == &previous, "event's pre-step snapshot changed");
        require(cache.get(6000, fetch) == &latest, "completed event snapshot changed");
    }
    require(fetches == 0, "admitted pair reacquired history per event");
}

void history_lives_for_one_frame() {
    const Snapshot previous{20, 1}, latest{21, 2};
    auto history = std::make_shared<const Snapshot>(Snapshot{3, 30});
    std::weak_ptr<const Snapshot> weak = history;
    unsigned fetches = 0;
    const auto fetch = [&](std::uint64_t) { ++fetches; return history; };
    {
        eawr::presentation::space::EventSnapshotCache cache(previous, latest);
        const auto* retained = cache.get(3, fetch);
        require(retained && retained->value == 30, "old event snapshot not read");
        history.reset();
        for (unsigned event = 0; event < 100; ++event)
            require(cache.get(3, fetch) == retained, "successful history read not reused");
        require(!weak.expired() && fetches == 1, "history was lost during event processing");
    }
    require(weak.expired(), "historical snapshot retained beyond the frame");
    history = std::make_shared<const Snapshot>(Snapshot{3, 31});
    eawr::presentation::space::EventSnapshotCache next_frame(previous, latest);
    require(next_frame.get(3, fetch)->value == 31 && fetches == 2, "new frame used stale history");
}

void misses_retry_and_sparse_ticks_are_keys() {
    const Snapshot previous{0, 1}, latest{0, 2};
    eawr::presentation::space::EventSnapshotCache cache(previous, latest);
    std::shared_ptr<const Snapshot> history;
    unsigned fetches = 0;
    const auto fetch = [&](std::uint64_t) { ++fetches; return history; };
    constexpr auto tick = std::numeric_limits<std::uint64_t>::max();
    require(cache.get(0, fetch) == &latest, "latest same-tick replacement did not win");
    require(cache.get(tick, fetch) == nullptr, "missing history invented a snapshot");
    require(cache.get(tick, fetch) == nullptr && fetches == 2, "missing history was cached");
    history = std::make_shared<const Snapshot>(Snapshot{tick, 42});
    require(cache.get(tick, fetch)->value == 42 && fetches == 3, "later available history was not admitted");
    history.reset();
    require(cache.get(tick, fetch)->value == 42 && fetches == 3, "sparse successful read was not retained");
}
void admitted_snapshots_survive_history_eviction() {
    const Snapshot previous{17, 170}, latest{18, 180};
    std::vector<std::shared_ptr<const Snapshot>> published_history;
    for (std::uint64_t tick = 19; tick < 83; ++tick)
        published_history.push_back(std::make_shared<const Snapshot>(Snapshot{tick, 0}));
    unsigned fetches = 0;
    const auto fetch = [&](const std::uint64_t tick) -> std::shared_ptr<const Snapshot> {
        ++fetches;
        for (const auto& snapshot : published_history)
            if (snapshot->completed_tick() == tick) return snapshot;
        return {};
    };
    require(!fetch(17) && !fetch(18), "fixture did not evict the admitted frame's pair");
    fetches = 0;
    eawr::presentation::space::EventSnapshotCache cache(previous, latest);
    require(cache.get(17, fetch) == &previous, "eviction lost the held hit-profile/pick snapshot");
    require(cache.get(18, fetch) == &latest, "eviction lost the held completed-event snapshot");
    require(fetches == 0, "evicted admitted snapshots consulted unavailable history");
}

void multiple_catchup_records_retain_distinct_history() {
    const Snapshot previous{99, 99}, latest{100, 100};
    std::vector<std::shared_ptr<const Snapshot>> history;
    std::vector<std::weak_ptr<const Snapshot>> weak;
    for (std::uint64_t tick = 40; tick <= 43; ++tick) {
        history.push_back(std::make_shared<const Snapshot>(Snapshot{tick, static_cast<int>(tick)}));
        weak.push_back(history.back());
    }
    unsigned fetches = 0;
    const auto fetch = [&](const std::uint64_t tick) -> std::shared_ptr<const Snapshot> {
        ++fetches;
        for (const auto& snapshot : history)
            if (snapshot->completed_tick() == tick) return snapshot;
        return {};
    };
    {
        eawr::presentation::space::EventSnapshotCache cache(previous, latest);
        // Three reached records share their pre/post-step boundaries: only four
        // distinct history reads are needed, regardless of repeated consumers.
        for (std::uint64_t record = 41; record <= 43; ++record) {
            require(cache.get(record - 1, fetch)->value == static_cast<int>(record - 1), "catch-up pre-step changed");
            require(cache.get(record, fetch)->value == static_cast<int>(record), "catch-up completed step changed");
        }
        require(fetches == 4, "shared catch-up boundaries reacquired history");
        history.clear(); // All four ticks leave the provider while this frame is still being consumed.
        for (std::uint64_t tick = 40; tick <= 43; ++tick)
            require(cache.get(tick, fetch)->value == static_cast<int>(tick), "eviction lost cached catch-up history");
        require(fetches == 4, "successful evicted history was reacquired");
        for (const auto& snapshot : weak) require(!snapshot.expired(), "catch-up owner expired too early");
    }
    for (const auto& snapshot : weak) require(snapshot.expired(), "catch-up owner escaped its frame");
    eawr::presentation::space::EventSnapshotCache next_frame(previous, latest);
    require(next_frame.get(40, fetch) == nullptr, "next frame reused an evicted catch-up snapshot");
}

} // namespace

int main() {
    try {
        admitted_pair_needs_no_history();
        admitted_snapshots_survive_history_eviction();
        multiple_catchup_records_retain_distinct_history();
        history_lives_for_one_frame();
        misses_retry_and_sparse_ticks_are_keys();
        std::cout << "event snapshot cache contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
