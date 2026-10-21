#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace eawr::sim::tactical {

// PC-07..09: a compile-time provider contract. Requests, search storage and
// checkpoints belong to the backend; the service never assumes a collision
// layer, terrain representation or checkpoint field layout. The backend's
// immutable content must outlive its searches, which own their mutable progress.
template <class Backend>
class PathService final {
public:
    using Search = typename Backend::Search;
    using Checkpoint = typename Backend::Checkpoint;
    using Stats = typename Backend::Stats;

    template <class... Args>
    [[nodiscard]] static Search begin(Args&&... args) {
        return Backend::begin(std::forward<Args>(args)...);
    }
    static bool step(Search& search, std::uint64_t budget, Stats* stats = nullptr) {
        return search.run(budget, stats);
    }
    [[nodiscard]] static auto result(const Search& search) { return search.result(); }
    [[nodiscard]] static bool ended(const Search& search) noexcept { return search.ended(); }
    [[nodiscard]] static bool exhausted(const Search& search) noexcept { return search.exhausted(); }
    [[nodiscard]] static std::uint64_t expansions(const Search& search) noexcept { return search.expansions(); }
    [[nodiscard]] static std::size_t held_bytes(const Search& search) noexcept { return search.held_bytes(); }
    [[nodiscard]] static Checkpoint checkpoint(const Search& search) { return search.checkpoint(); }
    template <class... Args>
    [[nodiscard]] static auto restore(Args&&... args) {
        return Backend::restore(std::forward<Args>(args)...);
    }
    template <class... Args>
    [[nodiscard]] static auto plan(Args&&... args) {
        return Backend::plan(std::forward<Args>(args)...);
    }
    template <class... Args>
    [[nodiscard]] static auto plan_within(Args&&... args) {
        return Backend::plan_within(std::forward<Args>(args)...);
    }

    [[nodiscard]] static std::uint64_t slice_limit(std::uint64_t frame, std::uint64_t landing,
        std::uint64_t slice) noexcept {
        return landing <= frame ? std::numeric_limits<std::uint64_t>::max() : slice;
    }

    // A tick owns one budget ledger, reused across command barriers. A batch
    // consumes one job per independent lane per round, committed in input order.
    // Workers only read remaining(); charge() is called by the ordered commit.
    template <std::size_t Count>
    class Lanes final {
    public:
        explicit Lanes(std::uint64_t budget) : budget_(budget) {}
        [[nodiscard]] std::uint64_t remaining(std::size_t lane) const noexcept {
            return spent_[lane] < budget_ ? budget_ - spent_[lane] : 0;
        }
        void charge(std::size_t lane, std::uint64_t expansions) noexcept { spent_[lane] += expansions; }

        class Schedule final {
        public:
            template <class Jobs, class LaneOf>
            Schedule(const Jobs& jobs, LaneOf lane_of) {
                for (std::size_t index = 0; index < jobs.size(); ++index) lanes_[lane_of(jobs[index])].push_back(index);
            }
            [[nodiscard]] std::vector<std::size_t> next() {
                std::vector<std::size_t> round;
                for (std::size_t lane = 0; lane < Count; ++lane) {
                    if (next_[lane] < lanes_[lane].size()) round.push_back(lanes_[lane][next_[lane]++]);
                }
                std::sort(round.begin(), round.end());
                return round;
            }
        private:
            std::array<std::vector<std::size_t>, Count> lanes_;
            std::array<std::size_t, Count> next_{};
        };
    private:
        std::uint64_t budget_{};
        std::array<std::uint64_t, Count> spent_{};
    };
};

} // namespace eawr::sim::tactical
